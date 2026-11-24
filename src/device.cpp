//
//  device.cpp
//  AMDRadeonRDNA4
//
//  Hardware side of AMDRadeonRDNA4 (see device.hpp). Moved out of the former
//  IOFramebuffer subclass unchanged in behaviour when AMDRadeonRDNA4 became a Lilu
//  plugin that extends Apple's IONDRVFramebuffer.
//

#include "device.hpp"
#include "klog.hpp"
#include "dpphy.hpp"
#include "edid.hpp"
#include "dmub.hpp"
#include "sku.hpp"
#include "trial.hpp"
#include "dpdual.hpp"
#include "dcninit.hpp"
#include "compute.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include <sys/sysctl.h>

#include "buildrev.h"   // AMDRADEONRDNA4_BUILD, written by the Makefile

#define FBLOG(fmt, ...)  rdna4Log("AMDRadeonRDNA4: " fmt "\n", ## __VA_ARGS__)

// ---------------------------------------------------------------------------
// Console framebuffer discovery
// ---------------------------------------------------------------------------

bool RDNA4Device::captureConsoleInfo() {
	IOPlatformExpert *platform = IOService::getPlatform();
	if (!platform) {
		FBLOG("no platform expert");
		return false;
	}

	PE_Video video {};
	IOReturn ret = platform->getConsoleInfo(&video);
	if (ret != kIOReturnSuccess) {
		FBLOG("getConsoleInfo failed 0x%x", ret);
		return false;
	}

	// v_baseAddr carries flag bits in its low bits (observed 0x840000001 on
	// this machine); the scanout base itself is page-aligned. Apple's own
	// framebuffer code masks these off — failing to do so hands WindowServer
	// an aperture shifted one byte from the real scanout, which recolours
	// every pixel (B'=prev alpha, G'=B, R'=G).
	fbPhysBase = static_cast<IOPhysicalAddress64>(video.v_baseAddr) & ~0xFFFULL;
	if (video.v_baseAddr & 0xFFF)
		FBLOG("console base 0x%lx carries flag bits, using 0x%llx",
		      video.v_baseAddr, fbPhysBase);
	fbWidth    = static_cast<uint32_t>(video.v_width);
	fbHeight   = static_cast<uint32_t>(video.v_height);
	fbRowBytes = static_cast<uint32_t>(video.v_rowBytes);
	fbDepth    = static_cast<uint32_t>(video.v_depth);

	// v_length may be 0, in which case it is rowBytes * height.
	fbLength = video.v_length ? static_cast<uint64_t>(video.v_length)
	                          : static_cast<uint64_t>(fbRowBytes) * fbHeight;

	if (fbPhysBase == 0 || fbWidth == 0 || fbHeight == 0 || fbRowBytes == 0) {
		FBLOG("console framebuffer looks invalid: base=0x%llx %ux%u stride=%u depth=%u",
		      fbPhysBase, fbWidth, fbHeight, fbRowBytes, fbDepth);
		return false;
	}

	// We only support a 32bpp linear framebuffer.
	if (fbDepth != 32 && fbDepth != 30 && fbDepth != 24) {
		FBLOG("unsupported console depth %u", fbDepth);
		return false;
	}
	fbDepth = 32;

	FBLOG("console framebuffer: base=0x%llx %ux%u stride=%u depth=%u len=%llu",
	      fbPhysBase, fbWidth, fbHeight, fbRowBytes, fbDepth, fbLength);

	// Publish what we adopted so `ioreg -lw0` on the target shows the
	// geometry without needing the kernel log.
	owner->setProperty("AMDRadeonRDNA4,Build", AMDRADEONRDNA4_BUILD);   // which build this is, where the kernel log's first lines may be gone
	owner->setProperty("Console,BaseAddress", fbPhysBase, 64);
	owner->setProperty("Console,Width", static_cast<uint64_t>(fbWidth), 32);
	owner->setProperty("Console,Height", static_cast<uint64_t>(fbHeight), 32);
	owner->setProperty("Console,RowBytes", static_cast<uint64_t>(fbRowBytes), 32);
	owner->setProperty("Console,Depth", static_cast<uint64_t>(fbDepth), 32);
	owner->setProperty("Console,Length", fbLength, 64);
	return true;
}

// ---------------------------------------------------------------------------
// VBIOS acquisition and parsing
// ---------------------------------------------------------------------------

// Cap on how much ROM we are willing to copy. The full Navi 48 flash is 2 MiB;
// the legacy image within it is ~58 KiB.
static constexpr size_t kMaxVBIOSSize = 2 * 1024 * 1024;

bool RDNA4Device::copyVBIOSFromProperty() {
	// OpenCore DeviceProperties (or WhateverGreen) can inject the VBIOS as
	// ATY,bin_image on the GPU's PCI node. Preferred: no hardware access.
	auto *rom = OSDynamicCast(OSData, pciDevice->getProperty("ATY,bin_image"));
	if (!rom || rom->getLength() < 512 || rom->getLength() > kMaxVBIOSSize)
		return false;

	vbiosSize = rom->getLength();
	vbiosData = static_cast<uint8_t *>(IOMalloc(vbiosSize));
	if (!vbiosData) {
		vbiosSize = 0;
		return false;
	}
	memcpy(vbiosData, rom->getBytesNoCopy(), vbiosSize);
	FBLOG("VBIOS: %zu bytes from ATY,bin_image", vbiosSize);
	return true;
}

bool RDNA4Device::copyVBIOSFromExpansionROM() {
	// Size the expansion ROM BAR, then map and copy it with decode enabled.
	uint32_t saved = pciDevice->configRead32(kIOPCIConfigExpansionROMBase);
	pciDevice->configWrite32(kIOPCIConfigExpansionROMBase, 0xFFFFF800);
	uint32_t sizing = pciDevice->configRead32(kIOPCIConfigExpansionROMBase);
	pciDevice->configWrite32(kIOPCIConfigExpansionROMBase, saved);

	uint32_t addr = saved & 0xFFFFF800;
	size_t romLen = sizing ? static_cast<size_t>(~(sizing & 0xFFFFF800)) + 1 : 0;
	if (!addr || !romLen || romLen > kMaxVBIOSSize) {
		FBLOG("VBIOS: expansion ROM unavailable (bar=0x%08x size=%zu)", saved, romLen);
		return false;
	}

	auto *desc = IOMemoryDescriptor::withPhysicalAddress(addr, romLen, kIODirectionIn);
	if (!desc)
		return false;

	bool ok = false;
	// Enable ROM decode only for the duration of the copy.
	pciDevice->configWrite32(kIOPCIConfigExpansionROMBase, addr | 1);
	auto *map = desc->map();
	if (map) {
		vbiosData = static_cast<uint8_t *>(IOMalloc(romLen));
		if (vbiosData) {
			memcpy(vbiosData, reinterpret_cast<const void *>(map->getVirtualAddress()), romLen);
			vbiosSize = romLen;
			ok = true;
		}
		map->release();
	}
	pciDevice->configWrite32(kIOPCIConfigExpansionROMBase, saved);
	desc->release();

	if (ok)
		FBLOG("VBIOS: %zu bytes from expansion ROM at 0x%08x", vbiosSize, addr);
	return ok;
}

void RDNA4Device::freeVBIOS() {
	if (vbiosData) {
		IOFree(vbiosData, vbiosSize);
		vbiosData = nullptr;
		vbiosSize = 0;
	}
}

void RDNA4Device::publishVBIOSInfo() {
	char name[64];
	if (atomBios.configName(name, sizeof(name)))
		owner->setProperty("AtomBIOS,ImageName", name);

	AtomBios::FirmwareInfo3 fw;
	if (atomBios.getFirmwareInfo(fw)) {
		owner->setProperty("AtomBIOS,FirmwareRevision", fw.firmwareRevision, 32);
		owner->setProperty("AtomBIOS,FirmwareCapability", fw.firmwareCapability, 32);
	}

	AtomBios::DisplayPath paths[AtomBios::MaxDisplayPaths];
	size_t n = atomBios.getDisplayPaths(paths, AtomBios::MaxDisplayPaths);
	if (n) {
		// e.g. "DisplayPort/ddc0/hpd1,DisplayPort/ddc1/hpd2,..."
		char list[192] {};
		for (size_t i = 0; i < n; i++) {
			const char *conn =
			    AtomBios::connectorName(AtomBios::connectorType(paths[i].connectorObjId));

			AtomBios::PathRecords rec {};
			atomBios.getPathRecords(paths[i], rec);

			char entry[48];
			snprintf(entry, sizeof(entry), "%s%s/ddc%u/hpd%u",
			         i ? "," : "", conn, rec.ddcLine, rec.hpdPin);
			strlcat(list, entry, sizeof(list));

			FBLOG("connector %zu: %s objid=0x%04x encoder=0x%04x ddc-line=%u hpd-pin=%u", i,
			      conn, paths[i].connectorObjId, paths[i].encoderObjId, rec.ddcLine, rec.hpdPin);
		}
		owner->setProperty("AtomBIOS,Connectors", list);
		owner->setProperty("AtomBIOS,ConnectorCount", static_cast<uint64_t>(n), 32);
	}
}

bool RDNA4Device::loadVBIOS() {
	if (!copyVBIOSFromProperty() && !copyVBIOSFromExpansionROM()) {
		FBLOG("VBIOS: no image available (inject ATY,bin_image via DeviceProperties)");
		return false;
	}

	if (!atomBios.init(vbiosData, vbiosSize)) {
		FBLOG("VBIOS: image failed AtomBIOS validation");
		freeVBIOS();
		return false;
	}

	FBLOG("VBIOS: valid AtomBIOS image at +0x%zx, %zu bytes",
	      atomBios.imageOffset(), atomBios.imageLength());
	publishVBIOSInfo();

	// IP discovery only exists in a full flash dump; a bare legacy VBIOS
	// (typical expansion ROM contents) won't have it. Non-fatal.
	if (ipDiscovery.init(vbiosData, vbiosSize)) {
		FBLOG("discovery: binary at +0x%zx, %u IPs", ipDiscovery.binaryOffset(),
		      ipDiscovery.ipCount());
		owner->setProperty("Discovery,Source", "VBIOS image");
		IpDiscovery::IpEntry gc, dmu;
		if (ipDiscovery.findIp(IpDiscovery::HwGc, 0, gc)) {
			char ver[16];
			snprintf(ver, sizeof(ver), "%u.%u.%u", gc.major, gc.minor, gc.revision);
			owner->setProperty("Discovery,GCVersion", ver);
		}
		if (ipDiscovery.findIp(IpDiscovery::HwDmu, 0, dmu)) {
			char ver[16];
			snprintf(ver, sizeof(ver), "%u.%u.%u", dmu.major, dmu.minor, dmu.revision);
			owner->setProperty("Discovery,DCNVersion", ver);
		}
	} else {
		FBLOG("discovery: not present in image (inject the full 2MiB flash dump to enable)");
	}

	// Command-function inventory for the mode-setting plan: amdgpu drives the
	// PHY/PLL through these bytecode routines rather than raw registers.
	static const struct { uint8_t idx; const char *name; } cmds[] = {
		{ AtomBios::CmdSetPixelClock,          "setpixelclock" },
		{ AtomBios::CmdDig1TransmitterControl, "dig1transmittercontrol" },
		{ AtomBios::CmdDigEncoderControl,      "digxencodercontrol" },
		{ AtomBios::CmdEnableCrtc,             "enablecrtc" },
		{ AtomBios::CmdSetCrtcUsingDtdTiming,  "setcrtc_usingdtdtiming" },
		{ AtomBios::CmdEnableDispPowerGating,  "enabledisppowergating" },
	};
	for (auto &c : cmds) {
		AtomBios::CmdTableInfo info;
		if (atomBios.getCommandTable(c.idx, info))
			FBLOG("cmd: %s v%u.%u %u bytes at +0x%zx", c.name,
			      info.formatRev, info.contentRev, info.size,
			      info.offset - atomBios.imageOffset());
		else
			FBLOG("cmd: %s absent", c.name);
	}
	return true;
}

bool RDNA4Device::loadOnDieDiscovery() {
	// The PSP keeps a copy of the IP discovery binary in a reserved region
	// at the top of VRAM (upstream: DISCOVERY_TMR_OFFSET = 64 KiB below the
	// end, DISCOVERY_TMR_SIZE = 10 KiB) — present on every powered-on card,
	// no ROM dump needed. It sits far outside the CPU aperture, so read it
	// through MM_INDEX/MM_DATA. Technique per lemonade-sdk/mac-amdgpu
	// (MIT) amdgpu_discovery.cpp and upstream amdgpu_discovery.c.
	constexpr uint32_t kTmrSize   = 10 << 10;
	constexpr uint32_t kTmrOffset = 64 << 10;
	constexpr uint32_t kMemsizeFallback = 0x378c; // NBIF RCC_CONFIG_MEMSIZE

	if (!rmmio)
		return false;
	uint32_t vramMB = regRead32(kMemsizeFallback);
	if (vramMB == 0 || vramMB == 0xFFFFFFFF) {
		FBLOG("discovery: on-die: VRAM size unreadable (0x%08x)", vramMB);
		return false;
	}

	onDieDisc = static_cast<uint8_t *>(IOMalloc(kTmrSize));
	if (!onDieDisc)
		return false;

	uint64_t pos = (static_cast<uint64_t>(vramMB) << 20) - kTmrOffset;
	uint32_t *dw = reinterpret_cast<uint32_t *>(onDieDisc);
	for (uint32_t i = 0; i < kTmrSize / 4; i++)
		dw[i] = vramRead32(pos + 4ULL * i);

	FBLOG("discovery: on-die TMR at vram+0x%llx, first dwords %08x %08x",
	      pos, dw[0], dw[1]);
	if (!ipDiscovery.init(onDieDisc, kTmrSize)) {
		FBLOG("discovery: on-die TMR did not validate");
		IOFree(onDieDisc, kTmrSize);
		onDieDisc = nullptr;
		return false;
	}

	FBLOG("discovery: on-die binary valid, %u IPs (no ROM injection needed)",
	      ipDiscovery.ipCount());
	IpDiscovery::IpEntry gc, dmu;
	char ver[16];
	if (ipDiscovery.findIp(IpDiscovery::HwGc, 0, gc)) {
		snprintf(ver, sizeof(ver), "%u.%u.%u", gc.major, gc.minor, gc.revision);
		owner->setProperty("Discovery,GCVersion", ver);
	}
	if (ipDiscovery.findIp(IpDiscovery::HwDmu, 0, dmu)) {
		snprintf(ver, sizeof(ver), "%u.%u.%u", dmu.major, dmu.minor, dmu.revision);
		owner->setProperty("Discovery,DCNVersion", ver);
	}
	owner->setProperty("Discovery,Source", "on-die TMR");
	return true;
}

// ---------------------------------------------------------------------------
// Register MMIO (BAR5)
// ---------------------------------------------------------------------------

bool RDNA4Device::mapRegisters() {
	// On AMD dGPUs since Bonaire the register aperture is BAR5 (BAR0/1 is the
	// VRAM aperture, BAR2/3 doorbells) — amdgpu_device.c does the same.
	IODeviceMemory *bar = pciDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
	if (!bar) {
		FBLOG("mmio: BAR5 not present/assigned");
		return false;
	}

	rmmioMap = bar->map();
	if (!rmmioMap) {
		FBLOG("mmio: failed to map BAR5");
		return false;
	}

	rmmio = reinterpret_cast<volatile uint32_t *>(rmmioMap->getVirtualAddress());
	rmmioSize = rmmioMap->getLength();
	FBLOG("mmio: BAR5 mapped, %zu KiB", rmmioSize / 1024);
	return true;
}

void RDNA4Device::unmapRegisters() {
	rmmio = nullptr;
	rmmioSize = 0;
	if (rmmioMap) {
		rmmioMap->release();
		rmmioMap = nullptr;
	}
}

uint32_t RDNA4Device::regRead32(uint32_t byteOffset) const {
	if (!rmmio || byteOffset + 4 > rmmioSize)
		return 0xFFFFFFFF;
	return rmmio[byteOffset / 4];
}

uint32_t RDNA4Device::regReadDmu(uint8_t baseIdx, uint32_t dwordOffset) const {
	uint32_t byteOffset;
	if (!ipDiscovery.isValid() ||
	    !ipDiscovery.regByteOffset(IpDiscovery::HwDmu, 0, baseIdx, dwordOffset, byteOffset))
		return 0xFFFFFFFF;
	return regRead32(byteOffset);
}

void RDNA4Device::dumpDCN() {
	if (!ipDiscovery.isValid()) {
		FBLOG("dcn: no discovery bases, skipping register dump");
		return;
	}

	// DCN 4.1.0 (Navi 48) output-pipe registers, pipe/instance 0.
	// {name, base_idx, dword offset} — offsets from Linux dcn_4_1_0_offset.h.
	// This is the console pipe the firmware lit up; if these read as identity
	// we will widen to pipes 1-3.
	// All pipe color blocks read identity, so the (G,B,R) rotation is not in
	// the DCN color pipe. Prime suspect now: the DP stream encoder emitting
	// YCbCr while the sink assumes RGB. Dump each DP encoder's pixel format
	// (DP_PIXEL_ENCODING: 0=RGB, 1=YCbCr422, 2=YCbCr444) and MSA colorimetry,
	// plus each HUBP surface config to identify the active pipe.
	struct DpEnc { uint32_t pixFmt, colorimetry; };
	static const DpEnc dp[4] = {
		{ 0x211f, 0x2120 }, { 0x2243, 0x2244 }, { 0x2367, 0x2368 }, { 0x248b, 0x248c },
	};
	static const uint32_t hubpCfg[4] = { 0x05e5, 0x06c1, 0x079d, 0x0879 };
	static const uint32_t digFeCntl[4] = { 0x2093, 0x21b7 /*+0x124*/, 0x22db, 0x23ff };

	FBLOG("dcn: register dump (encoders, read-only) ---");
	FBLOG("dcn:   OTG0_OTG_CONTROL = 0x%08x", regReadDmu(2, 0x1b43));
	for (int i = 0; i < 4; i++)
		FBLOG("dcn:   [%d] HUBP_surf=0x%08x DP_PIXEL_FORMAT=0x%08x DP_MSA_COLORIMETRY=0x%08x DIG_FE_CNTL=0x%08x",
		      i, regReadDmu(2, hubpCfg[i]), regReadDmu(2, dp[i].pixFmt),
		      regReadDmu(2, dp[i].colorimetry), regReadDmu(2, digFeCntl[i]));
	// MSA reads consistent (RGB 10bpc, MISC0=0x40 in COLORIMETRY[31:24]).
	// Remaining GPU-side suspect: secondary data packets (VSC/GSP) that DP1.3+
	// sinks may honor over the MSA. Dump the SDP enables and stream control.
	// Of the lit pipe's stream encoder (DP0 if no pipe was found).
	const uint32_t dig = digOff();
	const unsigned enc = dig / Pipe::Reg::kDigStride;
	FBLOG("dcn:   DP%u_DP_MSA_MISC = 0x%08x (MISC1..4)", enc, regReadDmu(2, 0x2124 + dig));
	FBLOG("dcn:   DP%u_DP_VID_STREAM_CNTL = 0x%08x", enc, regReadDmu(2, 0x2122 + dig));
	FBLOG("dcn:   DP%u_DP_SEC_CNTL  = 0x%08x (GSP/VSC/ASP enables)", enc, regReadDmu(2, 0x2141 + dig));
	FBLOG("dcn:   DP%u_DP_SEC_CNTL1 = 0x%08x", enc, regReadDmu(2, 0x2142 + dig));
	FBLOG("dcn:   DP%u_DP_SEC_CNTL2 = 0x%08x", enc, regReadDmu(2, 0x2169 + dig));
	FBLOG("dcn:   DP%u_DP_SEC_CNTL7 = 0x%08x", enc, regReadDmu(2, 0x216e + dig));
	FBLOG("dcn:   DP%u_DP_MSA_VBID_MISC = 0x%08x", enc, regReadDmu(2, 0x2170 + dig));

	// MPC MCM blocks (shaper -> 3DLUT -> 1DLUT), the post-CSC colour stages
	// not covered by earlier dumps. An enabled 3DLUT with unloaded RAM would
	// produce exactly the observed spatially-perfect arbitrary recolouring.
	static const uint32_t mcm[4][3] = {  // {SHAPER_CONTROL, 3DLUT_MODE, 1DLUT_CONTROL}, base 3
		{ 0x0453, 0x048a, 0x0493 },
		{ 0x0503, 0x053a, 0x0543 },
		{ 0x05b3, 0x05ea, 0x05f3 },
		{ 0x0663, 0x069a, 0x06a3 },
	};
	for (int i = 0; i < 4; i++)
		FBLOG("dcn:   MCM%d shaper=0x%08x 3dlut_mode=0x%08x 1dlut=0x%08x",
		      i, regReadDmu(3, mcm[i][0]), regReadDmu(3, mcm[i][1]),
		      regReadDmu(3, mcm[i][2]));
	FBLOG("dcn: --- end register dump ---");

	// Boot-arg "rdna4-lutbypass=1": force all MCM stages to bypass.
	// Bypass is the hardware's pass-through state, so this cannot make the
	// image worse than a wrong LUT; a reboot restores firmware state.
	uint32_t fix = 0;
	if (PE_parse_boot_argn("rdna4-lutbypass", &fix, sizeof(fix)) && fix != 0) {
		for (int i = 0; i < 4; i++) {
			regWriteDmu(3, mcm[i][0], 0);  // shaper: bypass
			regWriteDmu(3, mcm[i][1], 0);  // 3DLUT: bypass
			regWriteDmu(3, mcm[i][2], 0);  // 1DLUT: bypass
		}
		FBLOG("dcn: MCM shaper/3DLUT/1DLUT forced to bypass on all pipes");
	}
}

// ---------------------------------------------------------------------------
// DP AUX software engine (EDID / DDC over the AUX channel)
// ---------------------------------------------------------------------------

namespace {
// All AUX registers live in the DMU IP at base_idx 2. Engine `n` sits at
// kAuxBase0 + n*kAuxStride; the members below are dword offsets within one
// engine's block (regDP_AUXn_* from dcn_4_1_0_offset.h).
constexpr uint32_t kAuxBase0      = 0x16b2;  // regDP_AUX0_AUX_CONTROL
constexpr uint32_t kAuxStride     = 0x1c;    // dwords between DP_AUXn blocks
constexpr uint32_t kAuxControl    = 0x0;
constexpr uint32_t kAuxSwControl  = 0x1;
constexpr uint32_t kAuxArbControl = 0x2;
constexpr uint32_t kAuxIntControl = 0x3;
constexpr uint32_t kAuxSwStatus   = 0x4;
constexpr uint32_t kAuxSwData     = 0x6;

// Field masks/shifts (dcn_3_2_0_sh_mask.h; identical layout on dcn_4_1_0).
constexpr uint32_t kAuxEn                = 1u << 0;   // AUX_CONTROL.AUX_EN
constexpr uint32_t kAuxSwGo              = 1u << 0;   // AUX_SW_CONTROL.AUX_SW_GO
constexpr uint32_t kAuxWrBytesShift      = 16;        // AUX_SW_WR_BYTES [20:16]
constexpr uint32_t kAuxWrBytesMask       = 0x1fu << 16;
constexpr uint32_t kAuxRwStatShift       = 2;         // AUX_REG_RW_CNTL_STATUS [3:2]
constexpr uint32_t kAuxRwStatMask        = 0x3u << 2;
constexpr uint32_t kAuxUseReq            = 1u << 16;  // AUX_SW_USE_AUX_REG_REQ
constexpr uint32_t kAuxDoneUsingReg      = 1u << 17;  // AUX_SW_DONE_USING_AUX_REG
constexpr uint32_t kAuxDoneAck           = 1u << 1;   // INTERRUPT_CONTROL.AUX_SW_DONE_ACK
constexpr uint32_t kAuxSwDone            = 1u << 0;   // AUX_SW_STATUS.AUX_SW_DONE
constexpr uint32_t kAuxRxTimeoutState    = 0x7u << 4; // [6:4]
constexpr uint32_t kAuxRxTimeout         = 1u << 7;
constexpr uint32_t kAuxHpdDiscon         = 1u << 9;
constexpr uint32_t kAuxReplyCountShift   = 24;        // AUX_SW_REPLY_BYTE_COUNT [28:24]
constexpr uint32_t kAuxReplyCountMask    = 0x1fu << 24;
constexpr uint32_t kAuxDataRw            = 1u << 0;   // AUX_SW_DATA.AUX_SW_DATA_RW (1=read)
constexpr uint32_t kAuxDataShift         = 8;         // AUX_SW_DATA.AUX_SW_DATA [15:8]
constexpr uint32_t kAuxDataMask          = 0xffu << 8;
constexpr uint32_t kAuxAutoincDisable    = 1u << 31;  // AUX_SW_AUTOINCREMENT_DISABLE

// enum aux_transaction_action — already aligned into the command high nibble.
constexpr uint8_t kActI2CWriteMot = 0x40;
constexpr uint8_t kActI2CReadMot  = 0x50;
constexpr uint8_t kActI2CRead     = 0x10;
constexpr uint8_t kActDpWrite     = 0x80;  // native AUX (DPCD) write
constexpr uint8_t kActDpRead      = 0x90;  // native AUX (DPCD) read

// AUX_REG_RW_CNTL_STATUS grant codes.
constexpr uint32_t kSwCanAccess   = 1;
constexpr uint32_t kDmcuCanAccess = 2;

// AUX reply nibble (first reply byte >> 4).
constexpr int kReplyAck      = 0x0;  // AUX ACK + I2C ACK
constexpr int kReplyAuxDefer = 0x2;
constexpr int kReplyI2CDefer = 0x8;
constexpr int kAuxNoReply    = -2;   // the transaction ran and the sink did not answer

constexpr uint8_t kDdcSlave  = 0x50; // VESA DDC/EDID I2C address
constexpr uint8_t kDdcSegment = 0x30; // E-DDC segment pointer
constexpr uint8_t kAuxRetry  = 7;    // per-transaction defer retries
// A DPCD transfer waits out a deferring sink for about 50 ms in all, amdgpu's
// AUX_MAX_DEFER_TIMEOUT_MS: seven tries 500 us apart, the rest 1 ms apart.
constexpr uint8_t kDpcdDeferTries = 54;
} // namespace

uint32_t RDNA4Device::auxDword(uint8_t inst, uint32_t reg) const {
	return kAuxBase0 + static_cast<uint32_t>(inst) * kAuxStride + reg;
}

int RDNA4Device::auxTransaction(uint8_t inst, uint8_t action, uint32_t address,
                               const uint8_t *data, uint8_t len,
                               uint8_t *reply, uint8_t replyCap,
                               uint8_t *replyBytes) {
	if (replyBytes) *replyBytes = 0;
	auxWhy = "no register access";
	auxSts = 0;
	if (!ipDiscovery.isValid() || !rmmio)
		return -1;

	const uint32_t rCtl = auxDword(inst, kAuxControl);
	const uint32_t rArb = auxDword(inst, kAuxArbControl);
	const uint32_t rInt = auxDword(inst, kAuxIntControl);
	const uint32_t rSwc = auxDword(inst, kAuxSwControl);
	const uint32_t rSts = auxDword(inst, kAuxSwStatus);
	const uint32_t rDat = auxDword(inst, kAuxSwData);

	// Release helper: hand the engine back (mirrors dce_aux release_engine).
	auto release = [&]() {
		regWriteDmu(2, rArb,
		            (regReadDmu(2, rArb) & ~kAuxUseReq) | kAuxDoneUsingReg);
	};

	// --- acquire software access (dce_aux acquire_engine) ---
	uint32_t arb = regReadDmu(2, rArb);
	if (arb == 0xFFFFFFFF)
		return -1;
	auxSts = arb;
	auxWhy = "the firmware holds the AUX engine";
	if (((arb & kAuxRwStatMask) >> kAuxRwStatShift) == kDmcuCanAccess)
		return -1;  // the firmware microcontroller owns this engine
	uint32_t ctl = regReadDmu(2, rCtl);
	if (!(ctl & kAuxEn))                       // GOP normally leaves AUX enabled
		regWriteDmu(2, rCtl, ctl | kAuxEn);
	regWriteDmu(2, rArb, regReadDmu(2, rArb) | kAuxUseReq);
	arb = regReadDmu(2, rArb);
	auxSts = arb;
	if (((arb & kAuxRwStatMask) >> kAuxRwStatShift) != kSwCanAccess) {
		auxWhy = "the AUX engine was not granted";
		release();
		return -1;
	}

	// --- clear any stale completion, wait for the engine to be idle ---
	regWriteDmu(2, rInt, kAuxDoneAck);
	for (int i = 0; i < 100 && (regReadDmu(2, rSts) & kAuxSwDone); i++)
		IODelay(10);

	// --- build the request buffer (submit_channel_request) ---
	// header = 3 bytes (action+addr[19:0]); a data phase adds a length byte,
	// and writes append the payload. AUX_SW_WR_BYTES counts them all.
	// Command nibble bit 0 (byte bit 0x10) is the read flag: write actions
	// (I2C_WRITE 0x00, I2C_WRITE_MOT 0x40, DP_WRITE 0x80) clear it; reads
	// (I2C_READ 0x10, I2C_READ_MOT 0x50, DP_READ 0x90) set it.
	const bool isWrite = (action & 0x10) == 0;
	uint32_t length = len ? 4u : 3u;
	if (isWrite) length += len;
	regWriteDmu(2, rSwc,
	            (regReadDmu(2, rSwc) & ~kAuxWrBytesMask) |
	            ((length << kAuxWrBytesShift) & kAuxWrBytesMask));

	// FIFO writes: INDEX=0, RW=0. The first byte latches index 0 with
	// autoincrement disabled; clearing it for the rest advances the pointer.
	uint32_t v = 0;
	auto push = [&](bool first, uint8_t b) {
		v &= ~(kAuxDataMask | kAuxAutoincDisable | kAuxDataRw);
		v |= (static_cast<uint32_t>(b) << kAuxDataShift) & kAuxDataMask;
		if (first) v |= kAuxAutoincDisable;
		regWriteDmu(2, rDat, v);
	};
	push(true,  static_cast<uint8_t>(action | ((address >> 16) & 0x0f)));
	push(false, static_cast<uint8_t>((address >> 8) & 0xff));
	push(false, static_cast<uint8_t>(address & 0xff));
	if (len) push(false, static_cast<uint8_t>(len - 1));
	if (isWrite)
		for (uint8_t i = 0; i < len; i++)
			push(false, data ? data[i] : 0);

	// --- go ---
	regWriteDmu(2, rSwc, regReadDmu(2, rSwc) | kAuxSwGo);

	// --- wait for completion (get_channel_status) ---
	uint32_t sts = 0;
	bool done = false;
	for (int i = 0; i < 2000; i++) {   // ~20 ms ceiling; typically microseconds
		sts = regReadDmu(2, rSts);
		if (sts & kAuxSwDone) { done = true; break; }
		IODelay(10);
	}
	auxSts = sts;
	if (!done || (sts & kAuxHpdDiscon) ||
	    (sts & kAuxRxTimeout) || (sts & kAuxRxTimeoutState)) {
		auxWhy = !done ? "the engine did not finish" : (sts & kAuxHpdDiscon) ? "HPD low" : "no reply (timeout)";
		release();
		return kAuxNoReply;
	}
	uint32_t nbytes = (sts & kAuxReplyCountMask) >> kAuxReplyCountShift;
	auxWhy = nbytes ? "" : "empty reply";

	// --- read the reply (read_channel_reply) ---
	int replyCode = -1;
	if (nbytes >= 1) {
		// point the read cursor at byte 0; reads auto-advance from there
		regWriteDmu(2, rDat, kAuxAutoincDisable | kAuxDataRw);  // INDEX=0, RW=1
		uint32_t hdr = (regReadDmu(2, rDat) & kAuxDataMask) >> kAuxDataShift;
		replyCode = (hdr >> 4) & 0x0f;           // first byte is the reply header
		uint32_t dataBytes = nbytes - 1;
		for (uint32_t i = 0; i < dataBytes; i++) {
			uint32_t d = (regReadDmu(2, rDat) & kAuxDataMask) >> kAuxDataShift;
			if (reply && i < replyCap) reply[i] = static_cast<uint8_t>(d);
		}
		if (replyBytes)
			*replyBytes = static_cast<uint8_t>(dataBytes < replyCap ? dataBytes
			                                                        : replyCap);
	}

	release();
	return replyCode;
}

bool RDNA4Device::readEDID(uint8_t inst, uint8_t *edid, size_t count,
                          uint8_t start, uint8_t segment) {
	uint8_t rb = 0;

	// Blocks past the first two: the E-DDC segment pointer first (I2C 0x30), as Linux's drm_do_probe_ddc_edid
	// writes it; only for a segment above 0, which some sinks refuse.
	if (segment && auxTransaction(inst, kActI2CWriteMot, kDdcSegment, &segment, 1, nullptr, 0, &rb) != kReplyAck)
		return false;

	// Point the sink's read pointer at `start` with an I2C write. MOT keeps
	// the I2C START asserted across the reads that follow.
	bool ok = false;
	for (uint8_t t = 0; t < kAuxRetry; t++) {
		int rc = auxTransaction(inst, kActI2CWriteMot, kDdcSlave, &start, 1,
		                        nullptr, 0, &rb);
		if (rc == kReplyAck) { ok = true; break; }
		if (rc < 0) return false;
		if (rc == kReplyAuxDefer || rc == kReplyI2CDefer) { IODelay(500); continue; }
		return false;  // NACK
	}
	if (!ok) return false;

	// Sequential 16-byte I2C reads (the AUX data FIFO limit); drop MOT on the
	// final chunk to issue the I2C STOP.
	for (size_t pos = 0; pos < count; ) {
		uint8_t chunk = (count - pos) > 16 ? 16 : static_cast<uint8_t>(count - pos);
		bool last = (pos + chunk) >= count;
		uint8_t act = last ? kActI2CRead : kActI2CReadMot;
		bool got = false;
		for (uint8_t t = 0; t < kAuxRetry; t++) {
			int rc = auxTransaction(inst, act, kDdcSlave, nullptr, chunk,
			                        edid + pos, chunk, &rb);
			if (rc == kReplyAck) { got = true; break; }
			if (rc < 0) return false;
			if (rc == kReplyAuxDefer || rc == kReplyI2CDefer) { IODelay(500); continue; }
			return false;
		}
		if (!got || rb == 0) return false;
		pos += rb;  // advance by what the sink actually returned
	}
	return true;
}

// The extension blocks past the first (E-DDC segments) into edidMore, in order, logged with their DisplayID timings:
// the mode table takes them, macOS's EDID stays the base block and the first extension.
void RDNA4Device::readMoreEdidBlocks(uint8_t inst, uint8_t blocks) {
	edidMoreLen = 0;
	for (uint8_t b = 2; b <= blocks && b <= 3; b++) {
		uint8_t *block = edidMore + edidMoreLen;
		if (!readEDID(inst, block, 128, static_cast<uint8_t>((b & 1) * 128), static_cast<uint8_t>(b / 2))) {
			FBLOG("edid: AUX%u: extension block %u not read (segment %u)", inst, b, b / 2);
			break;
		}
		edidMoreLen += 128;
		Edid::DetailedTiming t[8];
		const size_t n = Edid::displayIdTimings(block, t, 8);
		FBLOG("edid: AUX%u: extension block %u (tag 0x%02x, checksum %s): %lu DisplayID timing(s)", inst, b, block[0],
		      Edid::blockChecksumOk(block) ? "ok" : "BAD", static_cast<unsigned long>(n));
		for (size_t i = 0; i < n; i++)
			FBLOG("edid:   %ux%u@%u.%03u %u kHz h(blank=%u fp=%u sw=%u) v(blank=%u fp=%u sw=%u)", t[i].hActive,
			      t[i].vActive, t[i].refreshMilliHz() / 1000, t[i].refreshMilliHz() % 1000, t[i].pixelClockKHz,
			      t[i].hBlank, t[i].hSyncOffset, t[i].hSyncWidth, t[i].vBlank, t[i].vSyncOffset, t[i].vSyncWidth);
	}
}

// Native AUX: up to 16 bytes a transfer, a deferred one asked again.
bool RDNA4Device::dpcdRead(uint8_t aux, uint32_t address, uint8_t *data, size_t len) {
	for (size_t pos = 0; pos < len; ) {
		const uint8_t chunk = len - pos > 16 ? 16 : static_cast<uint8_t>(len - pos);
		uint8_t got = 0;
		int rc = kReplyAuxDefer;
		uint8_t timeouts = 0;
		for (uint8_t t = 0; t < kDpcdDeferTries && rc == kReplyAuxDefer; t++) {
			rc = auxTransaction(aux, kActDpRead, address + static_cast<uint32_t>(pos), nullptr, chunk,
			                    data + pos, chunk, &got);
			if (rc == kAuxNoReply && timeouts++ < 3)
				rc = kReplyAuxDefer;                // dce_aux_transfer_with_retries: AUX_MAX_TIMEOUT_RETRIES
			// The Samsung answers a native transfer with the I2C defer code for a
			// while after its link dropped (card, 2026-10-06); amdgpu waits out both.
			if (rc == kReplyI2CDefer)
				rc = kReplyAuxDefer;
			if (rc == kReplyAuxDefer)
				IODelay(t < kAuxRetry ? 500 : 1000);
		}
		if (rc != kReplyAck || got == 0) {
			FBLOG("aux: AUX%u DPCD read 0x%03x (%u byte(s)) failed: %s (reply %d, status 0x%08x)", aux,
			      address + static_cast<uint32_t>(pos), chunk, rc == kAuxNoReply || rc == -1 ? auxWhy : "refused", rc,
			      auxSts);
			return false;
		}
		pos += got;   // a sink may answer with fewer bytes than asked
	}
	return true;
}

bool RDNA4Device::dpcdWrite(uint8_t aux, uint32_t address, const uint8_t *data, size_t len) {
	for (size_t pos = 0; pos < len; ) {
		const uint8_t chunk = len - pos > 16 ? 16 : static_cast<uint8_t>(len - pos);
		int rc = kReplyAuxDefer;
		uint8_t timeouts = 0;
		for (uint8_t t = 0; t < kDpcdDeferTries && rc == kReplyAuxDefer; t++) {
			rc = auxTransaction(aux, kActDpWrite, address + static_cast<uint32_t>(pos), data + pos, chunk,
			                    nullptr, 0, nullptr);
			if (rc == kAuxNoReply && timeouts++ < 3)
				rc = kReplyAuxDefer;                // dce_aux_transfer_with_retries: AUX_MAX_TIMEOUT_RETRIES
			// The Samsung answers a native transfer with the I2C defer code for a
			// while after its link dropped (card, 2026-10-06); amdgpu waits out both.
			if (rc == kReplyI2CDefer)
				rc = kReplyAuxDefer;
			if (rc == kReplyAuxDefer)
				IODelay(t < kAuxRetry ? 500 : 1000);
		}
		if (rc != kReplyAck) {
			FBLOG("aux: AUX%u DPCD write 0x%03x (%u byte(s)) failed: %s (reply %d, status 0x%08x)", aux,
			      address + static_cast<uint32_t>(pos), chunk, rc == kAuxNoReply || rc == -1 ? auxWhy : "refused", rc,
			      auxSts);
			return false;
		}
		pos += chunk;
	}
	return true;
}

void RDNA4Device::probeEDID() {
	// Default-on since the AUX path was verified on hardware (2026-07-11);
	// "rdna4-noedid=1" opts out if a sink misbehaves.
	uint32_t noedid = 0;
	if (PE_parse_boot_argn("rdna4-noedid", &noedid, sizeof(noedid)) && noedid != 0) {
		FBLOG("edid: probing disabled by rdna4-noedid");
		return;
	}
	if (!ipDiscovery.isValid() || !rmmio) {
		FBLOG("edid: MMIO/discovery unavailable, skipping");
		return;
	}

	// Hot-plug detect pin states (DC_GPIO_HPD_Y, hpd1-4 at bits 0/8/16/24):
	// distinguishes "no monitor asserting presence" from engine failures.
	uint32_t hpdY = regReadDmu(2, 0x28f7);
	FBLOG("edid: HPD_Y=0x%08x (hpd1=%u hpd2=%u hpd3=%u hpd4=%u)", hpdY,
	      (hpdY >> 0) & 1, (hpdY >> 8) & 1, (hpdY >> 16) & 1, (hpdY >> 24) & 1);
	auto hpdHigh = [&](uint8_t pin) {
		return pin >= 1 && pin <= 4 && ((hpdY >> (8 * (pin - 1))) & 1);
	};

	AtomBios::DisplayPath paths[AtomBios::MaxDisplayPaths];
	size_t n = atomBios.getDisplayPaths(paths, AtomBios::MaxDisplayPaths);
	bool any = false;
	for (size_t i = 0; i < n; i++) {
		AtomBios::ConnectorType ct =
		    AtomBios::connectorType(paths[i].connectorObjId);
		// DisplayPort/USB-C sinks carry DDC on the AUX channel; HDMI/DVI
		// EDID travels over the DC_I2C hardware engine.
		bool viaAux = (ct == AtomBios::ConnectorDP || ct == AtomBios::ConnectorUSBC);
		bool viaI2c = (ct == AtomBios::ConnectorHDMIA || ct == AtomBios::ConnectorDVID);
		if (!viaAux && !viaI2c)
			continue;

		AtomBios::PathRecords rec {};
		atomBios.getPathRecords(paths[i], rec);
		uint8_t inst = rec.ddcLine;  // AUX engine / DDC line index

		// A DisplayPort connector behind a passive DP++ (DP-to-HDMI)
		// adapter carries TMDS, and the sink's DDC is on the AUX pads in
		// I2C mode. For the boot display the lit pipe says so.
		const bool bootDpDual = viaAux && pipe.valid() && pipe.isTmds() &&
		                        rec.hasHpd && rec.hpdPin == pipe.hpd;

		if (bootDpDual) {
			FBLOG("edid: connector %zu: TMDS on a DisplayPort connector "
			      "(DP++ adapter): DDC%u in I2C mode", i, inst);
			ddcPadMode(inst, true);
			viaAux = false;
			setDpDual(rec.hpdPin, true);
		}

		const char *bus = viaAux ? "AUX" : "DDC";
		uint8_t edid[128] {};
		bool got = viaAux ? readEDID(inst, edid, sizeof(edid), 0)
		                  : readEDIDI2C(inst, edid, sizeof(edid), 0);

		// Another DP connector with a sink that is silent on AUX gets the
		// same try on DDC.
		if (!got && viaAux && rec.hasHpd && hpdHigh(rec.hpdPin)) {
			got = readEDIDDpDual(inst, edid, sizeof(edid));
			if (got) {
				FBLOG("edid: connector %zu: no AUX reply, the sink "
				      "answers on DDC%u (DP++ adapter)", i, inst);
				viaAux = false;
				bus = "DDC";
				setDpDual(rec.hpdPin, true);
			}
		}

		if (!got) {
			FBLOG("edid: connector %zu (%s%u): no reply", i, bus, inst);
			continue;
		}

		if (rec.hasHpd && dpDual(rec.hpdPin))
			readDpDualAdaptor(rec.hpdPin, inst);

		static const uint8_t sig[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
		if (memcmp(edid, sig, sizeof(sig)) != 0) {
			FBLOG("edid: connector %zu (%s%u): data but bad header "
			      "%02x %02x %02x %02x", i, bus, inst, edid[0], edid[1], edid[2], edid[3]);
			continue;
		}
		any = true;

		// Manufacturer id: bytes 8-9 big-endian, three 5-bit letters (1=A).
		uint16_t m = static_cast<uint16_t>((edid[8] << 8) | edid[9]);
		char mfg[4] = {
			static_cast<char>('@' + ((m >> 10) & 0x1f)),
			static_cast<char>('@' + ((m >> 5) & 0x1f)),
			static_cast<char>('@' + (m & 0x1f)), 0 };
		uint16_t product = static_cast<uint16_t>(edid[10] | (edid[11] << 8));
		FBLOG("edid: connector %zu (%s%u): %s product 0x%04x EDID %u.%u",
		      i, bus, inst, mfg, product, edid[18], edid[19]);

		// Preferred timing, fully decoded: these are the raster numbers an
		// OTG would be programmed with to drive this sink.
		Edid::DetailedTiming t {};
		if (Edid::parseDetailedTiming(edid + 54, t)) {
			FBLOG("edid: connector %zu (%s%u): preferred %ux%u@%u.%03uHz "
			      "pclk=%ukHz h(blank=%u fp=%u sw=%u %c) v(blank=%u fp=%u sw=%u %c)%s",
			      i, bus, inst, t.hActive, t.vActive,
			      t.refreshMilliHz() / 1000, t.refreshMilliHz() % 1000,
			      t.pixelClockKHz,
			      t.hBlank, t.hSyncOffset, t.hSyncWidth, t.hSyncPositive ? '+' : '-',
			      t.vBlank, t.vSyncOffset, t.vSyncWidth, t.vSyncPositive ? '+' : '-',
			      t.interlaced ? " interlaced" : "");
		}

		char key[40];
		snprintf(key, sizeof(key), "EDID,%s%u", bus, inst);
		owner->setProperty(key, edid, sizeof(edid));
		snprintf(key, sizeof(key), "EDID,%s%u-Vendor", bus, inst);
		owner->setProperty(key, mfg);

		// Cache the boot display's EDID for hasDDCConnect()/getDDCBlock(): the
		// sink on the lit pipe's connector, or the first that answers if the
		// pipe is not known. Another sink is the second display's.
		const bool boot = pipe.valid() && pipe.hpd ? rec.hasHpd && rec.hpdPin == pipe.hpd : edidLen == 0;
		if (boot && edidLen == 0) {
			memcpy(edidData, edid, 128);
			edidLen = 128;
			if (viaAux) {   // DPCD power writes only exist on AUX sinks
				sinkAuxInst = inst;
				sinkAuxValid = true;
			}
			// One CTA extension block is the norm on EDID 1.4 sinks; fetch it
			// so the OS sees the full timing/audio capabilities.
			bool ext = edid[126] > 0 &&
			           (viaAux ? readEDID(inst, edidData + 128, 128, 128)
			                   : readEDIDI2C(inst, edidData + 128, 128, 128));
			if (ext) {
				edidLen = 256;
				FBLOG("edid: connector %zu (%s%u): read extension block "
				      "(tag 0x%02x)", i, bus, inst, edidData[128]);
			}
			if (ext && viaAux && edid[126] > 1)
				readMoreEdidBlocks(inst, edid[126]);
		} else if (!boot && freeExtraSlot() < extraHeads()) {
			selectExtra(freeExtraSlot());
			memcpy(xp().edid, edid, 128);
			xp().edidLen = 128;
			xp().hpd = rec.hasHpd ? rec.hpdPin : 0;
			if (viaAux)
				readSecondDpSink(inst);
			// The extension says whether the sink has SCDC (Edid::hdmi2Caps).
			if (edid[126] > 0 && (viaAux ? readEDID(inst, xp().edid + 128, 128, 128)
			                             : readEDIDI2C(inst, xp().edid + 128, 128, 128)))
				xp().edidLen = 256;
		}
	}
	selectExtra(0);
	if (!any)
		FBLOG("edid: no sink EDID read on any connector");
	// The lit pipe's connector gave nothing but another did: the pipe's HPD
	// pin was read wrong, or its sink does not answer. As before the pipe was
	// consulted, the sink that answered is taken for the boot display.
	if (edidLen == 0 && xp().edidLen != 0) {
		FBLOG("edid: no EDID on the lit pipe's connector (HPD%u): taking the sink on HPD%u for the boot display",
		      pipe.hpd, xp().hpd);
		memcpy(edidData, xp().edid, sizeof(edidData));
		edidLen = xp().edidLen;
		xp().edidLen = 0;
		xp().hpd = 0;
		xp().sink = {};
	}
}

namespace {
// DC_I2C hardware engine (dcn_4_1_0_offset.h, all base_idx 2). One shared
// engine; per-DDC-line SETUP/SPEED registers plus a DDC_SELECT mux.
constexpr uint32_t kI2cControl     = 0x1e98;
constexpr uint32_t kI2cArbitration = 0x1e99;
constexpr uint32_t kI2cSwStatus    = 0x1e9b;
constexpr uint32_t kI2cSpeedBase   = 0x1ea2;  // + 2*line
constexpr uint32_t kI2cSetupBase   = 0x1ea3;  // + 2*line
constexpr uint32_t kI2cTxn0        = 0x1eae;  // txn N at +N
constexpr uint32_t kI2cDataReg     = 0x1eb2;

// DC_I2C_CONTROL fields.
constexpr uint32_t kI2cGo             = 1u << 0;
constexpr uint32_t kI2cSoftReset      = 1u << 1;
constexpr uint32_t kI2cSwStatusReset  = 1u << 3;
constexpr uint32_t kI2cDdcSelectShift = 8;       // [10:8]
constexpr uint32_t kI2cTxnCountShift  = 20;      // [21:20]
// DC_I2C_ARBITRATION fields.
constexpr uint32_t kI2cRwStatusShift  = 2;       // [3:2]: 0 idle, 1 SW, 2 HW
constexpr uint32_t kI2cRwStatusMask   = 0x3u << 2;
constexpr uint32_t kI2cNoQueuedSwGo   = 1u << 4;
constexpr uint32_t kI2cSwUseReq       = 1u << 20;
constexpr uint32_t kI2cSwDoneUsing    = 1u << 21;
// DC_I2C_SW_STATUS fields.
constexpr uint32_t kI2cSwDone         = 1u << 2;
constexpr uint32_t kI2cSwAborted      = 1u << 4;
constexpr uint32_t kI2cSwTimeout      = 1u << 5;
constexpr uint32_t kI2cSwOverflow     = 1u << 7;
constexpr uint32_t kI2cStoppedOnNack  = 1u << 8;
// DC_I2C_DDCx_SETUP fields (DCN values: TIME_LIMIT 3, 9-bit send-reset).
constexpr uint32_t kI2cSetupClkEn     = 1u << 3;   // dcn4.1: engine clock gate
constexpr uint32_t kI2cSetupEnable    = 1u << 6;
constexpr uint32_t kI2cSetupValue     = kI2cSetupEnable | kI2cSetupClkEn |
                                        (1u << 2) | (3u << 24);
// DIO memory low-power control: I2C engine RAM must be out of light sleep
// before the engine responds (DIO_MEM_PWR_CTRL/STATUS bit 0).
constexpr uint32_t kDioMemPwrStatus   = 0x1edd;
constexpr uint32_t kDioMemPwrCtrl     = 0x1ede;
// DC_I2C_DDCx_SPEED fields.
constexpr uint32_t kI2cSpeed100kHz    = 100;     // kHz, amdgpu's DCN default
// DC_I2C_TRANSACTIONx fields.
constexpr uint32_t kTxnRead           = 1u << 0;
constexpr uint32_t kTxnStopOnNack     = 1u << 8;
constexpr uint32_t kTxnStart          = 1u << 12;
constexpr uint32_t kTxnStop           = 1u << 13;
constexpr uint32_t kTxnCountShift     = 16;      // [25:16]
// DC_I2C_DATA fields.
constexpr uint32_t kI2cDataRead       = 1u << 0;
constexpr uint32_t kI2cDataShiftI2C   = 8;       // [15:8]
constexpr uint32_t kI2cIndexShift     = 16;      // [25:16]
constexpr uint32_t kI2cIndexWrite     = 1u << 31;

// MICROSECOND_TIME_BASE_DIV (DMU base_idx 1): reference for SCL prescale.
constexpr uint32_t kMicrosecondTimeBaseDiv = 0x007b;

// The DDC pads of DDC line N (dcn_4_1_0_offset.h / _sh_mask.h, base_idx 2),
// which hw_ddc.c set_config switches between AUX and I2C mode with DCN401's
// DDC_REG_LIST_DCN2.
constexpr uint32_t kGpioDdcMask       = 0x28d0;  // DC_GPIO_DDC1_MASK; DDCn at + 4 * line
constexpr uint32_t kGpioDdcClkPdEn    = 1u << 4;
constexpr uint32_t kGpioDdcDataPdEn   = 1u << 12;
constexpr uint32_t kGpioDdcPdEn       = kGpioDdcClkPdEn | kGpioDdcDataPdEn;
constexpr uint32_t kGpioDdcAuxPadMode = 1u << 16;
constexpr uint32_t kGpioAuxCtrl5      = 0x291d;  // DDC_PADn_I2CMODE at bit 12 + line
constexpr uint32_t kPhyAuxCntl        = 0x28ff;  // AUXn_PAD_RXSEL at [11:10] + 2 * line

constexpr uint32_t ddcI2cModeBit(uint8_t line) { return 1u << (12 + line); }
constexpr uint32_t auxRxselShift(uint8_t line) { return 10 + 2u * line; }
} // namespace

void RDNA4Device::ddcPadMode(uint8_t line, bool i2c) {
	if (line > 3 || !ipDiscovery.isValid() || !rmmio)
		return;

	const uint32_t rMask = kGpioDdcMask + 4u * line;
	const uint32_t was = regReadDmu(2, rMask);
	if (was == 0xFFFFFFFF)
		return;

	if (!i2c) {
		if (!(was & kGpioDdcAuxPadMode))
			regWriteDmu(2, rMask, was | kGpioDdcAuxPadMode);

		const uint32_t ctrl5 = regReadDmu(2, kGpioAuxCtrl5);
		regWriteDmu(2, kGpioAuxCtrl5, ctrl5 & ~ddcI2cModeBit(line));
		return;
	}

	// The pull-downs discharge what a plug-in left on the pads (2.5 ms),
	// then the pads leave AUX mode.
	if ((was & kGpioDdcPdEn) != kGpioDdcPdEn) {
		regWriteDmu(2, rMask, was | kGpioDdcPdEn);
		IOSleep(3);
	}

	if (regReadDmu(2, rMask) & kGpioDdcAuxPadMode) {
		IOSleep(2);
		regWriteDmu(2, rMask, regReadDmu(2, rMask) & ~kGpioDdcAuxPadMode);
	}

	const uint32_t ctrl5 = regReadDmu(2, kGpioAuxCtrl5);
	regWriteDmu(2, kGpioAuxCtrl5, ctrl5 | ddcI2cModeBit(line));

	// AUXn_PAD_RXSEL = 1: the pad receiver for I2C.
	const uint32_t shift = auxRxselShift(line);
	const uint32_t cntl = regReadDmu(2, kPhyAuxCntl);
	regWriteDmu(2, kPhyAuxCntl, (cntl & ~(3u << shift)) | (1u << shift));

	FBLOG("ddc: line %u in I2C mode: DDC%u_MASK 0x%08x -> 0x%08x, "
	      "AUX_CTRL_5 0x%08x, PHY_AUX_CNTL 0x%08x", line, line + 1u, was,
	      regReadDmu(2, rMask), regReadDmu(2, kGpioAuxCtrl5),
	      regReadDmu(2, kPhyAuxCntl));
}

// A passive DP++ adapter's ID block (dpdual.hpp): the TMDS clock it carries
// caps the modes of the display on that pin.
void RDNA4Device::readDpDualAdaptor(uint8_t hpd, uint8_t line) {
	if (hpd < 1 || hpd > 8)
		return;

	uint8_t id[DpDual::kIdBytes] {};
	const uint8_t start = 0;
	const uint8_t addr = static_cast<uint8_t>(DpDual::kI2cAddress << 1);
	const bool got = i2cTransfer(line, addr, &start, 1, id, sizeof(id));
	const DpDual::Adaptor a = DpDual::parse(id, got ? sizeof(id) : 0);

	dpDualKHz[hpd - 1] = a.maxTmdsKHz;

	if (a.type2)
		FBLOG("dp++: HPD%u: type 2 adapter %s, TMDS up to %u kHz", hpd,
		      a.deviceId, a.maxTmdsKHz);
	else
		FBLOG("dp++: HPD%u: %s, TMDS up to %u kHz", hpd,
		      got ? "type 1 adapter" : "no adapter ID", a.maxTmdsKHz);

	overrideDpDual(hpd);
}

// rdna4-dpdual-khz=<kHz>: what the DP++ adapters are taken to carry instead
// of what their ID block says. The test rig's CS5216 adapters report 300 MHz,
// and that is what they carry: a Dell lit at 3840x2160@60 (594 MHz) through
// one showed a broken picture (2026-11-04), so the default is the adapter's.
void RDNA4Device::overrideDpDual(uint8_t hpd) {
	uint32_t khz = 0;

	if (!Trial::option("rdna4-dpdual-khz", khz) || !khz)
		return;

	if (khz > DpDual::kType2MaxKHz)
		khz = DpDual::kType2MaxKHz;

	FBLOG("dp++: HPD%u: taken to carry %u kHz", hpd, khz);
	dpDualKHz[hpd - 1] = khz;
}

// The EDID of a sink behind a passive DP++ adapter: over DDC, with the
// connector's pads in I2C mode. They go back to AUX mode if it is silent.
bool RDNA4Device::readEDIDDpDual(uint8_t line, uint8_t *edid, size_t count) {
	ddcPadMode(line, true);
	if (readEDIDI2C(line, edid, count, 0))
		return true;

	ddcPadMode(line, false);
	return false;
}

bool RDNA4Device::readEDIDI2C(uint8_t line, uint8_t *edid, size_t count,
                             uint8_t start) {
	return count != 0 && i2cTransfer(line, kDdcSlave << 1, &start, 1, edid, count);
}

// One DDC exchange on the DC_I2C hardware engine: write `wlen` bytes to the
// device at 8-bit address `addr`, then (with `rlen`) read `rlen` bytes back
// after a repeated start.
bool RDNA4Device::i2cTransfer(uint8_t line, uint8_t addr, const uint8_t *wr, size_t wlen,
                              uint8_t *rd, size_t rlen) {
	if (!ipDiscovery.isValid() || !rmmio || wlen == 0 || wlen > 8 || rlen > 256)
		return false;

	const uint32_t rSetup = kI2cSetupBase + 2u * line;
	const uint32_t rSpeed = kI2cSpeedBase + 2u * line;

	// --- wake the engine BEFORE acquiring (dce_i2c_hw setup_engine order).
	// A never-used engine sits in soft reset with its registers write-blocked
	// (arbitration reads 0 and ignores requests — observed on hardware), and
	// its RAM may be in light sleep.
	regWriteDmu(2, kI2cControl, 0);                 // deassert DC_I2C_SOFT_RESET
	uint32_t memPwr = regReadDmu(2, kDioMemPwrCtrl);
	if (memPwr != 0xFFFFFFFF && (memPwr & 1)) {
		regWriteDmu(2, kDioMemPwrCtrl, memPwr & ~1u);  // unforce light sleep
		for (int i = 0; i < 10 && (regReadDmu(2, kDioMemPwrStatus) & 1); i++)
			IODelay(1);
	}
	regWriteDmu(2, rSetup, regReadDmu(2, rSetup) | kI2cSetupClkEn);

	// --- acquire (dce_i2c_hw acquire_engine) ---
	uint32_t arb = regReadDmu(2, kI2cArbitration);
	if (arb == 0xFFFFFFFF)
		return false;
	uint32_t owner = (arb & kI2cRwStatusMask) >> kI2cRwStatusShift;
	if (owner == 2) {        // hardware/DMCU owns the engine
		FBLOG("i2c: line %u: engine owned by HW/DMCU (arb=0x%08x)", line, arb);
		return false;
	}
	if (owner != 1) {
		regWriteDmu(2, kI2cArbitration, arb | kI2cSwUseReq);
		arb = regReadDmu(2, kI2cArbitration);
		if (((arb & kI2cRwStatusMask) >> kI2cRwStatusShift) != 1) {
			FBLOG("i2c: line %u: SW acquire not granted (arb=0x%08x)", line, arb);
			return false;
		}
	}
	auto release = [&]() {
		// Soft reset is safe while SW owns the engine; DONE_USING clears
		// the SW request and hands the engine back.
		regWriteDmu(2, kI2cControl, kI2cSoftReset | kI2cSwStatusReset);
		regWriteDmu(2, rSetup, 0);
		regWriteDmu(2, kI2cArbitration,
		            regReadDmu(2, kI2cArbitration) | kI2cSwDoneUsing);
	};

	// --- engine setup (setup_engine) ---
	regWriteDmu(2, rSetup, kI2cSetupValue);
	regWriteDmu(2, kI2cControl,
	            kI2cSwStatusReset | (static_cast<uint32_t>(line) << kI2cDdcSelectShift));
	regWriteDmu(2, kI2cArbitration,
	            (regReadDmu(2, kI2cArbitration) | kI2cSwUseReq) & ~kI2cNoQueuedSwGo);

	// SCL speed: prescale from the microsecond time base (set_speed). If the
	// time base is unprogrammed, keep whatever speed the firmware left.
	uint32_t mtb = regReadDmu(1, kMicrosecondTimeBaseDiv);
	uint32_t refBase = mtb & 0x7f, xtalDiv = (mtb >> 8) & 0x7f;
	if (mtb != 0xFFFFFFFF && refBase != 0) {
		if (xtalDiv == 0)
			xtalDiv = 2;
		uint32_t prescale = (refBase * 1000 / xtalDiv) / kI2cSpeed100kHz;
		regWriteDmu(2, rSpeed, (prescale << 16) | (2u << 8) | 2u);
	}

	// --- queue the transactions, then a single GO (process_transaction /
	// execute_transaction): [START addr <wr>] [START addr|1 read*rlen STOP],
	// or the write alone with its own STOP.
	regWriteDmu(2, kI2cTxn0,
	            kTxnStopOnNack | kTxnStart | (rlen ? 0 : kTxnStop) |
	            (static_cast<uint32_t>(wlen) << kTxnCountShift));
	if (rlen)
		regWriteDmu(2, kI2cTxn0 + 1,
		            kTxnStopOnNack | kTxnStart | kTxnRead | kTxnStop |
		            (static_cast<uint32_t>(rlen) << kTxnCountShift));
	// Data FIFO: address bytes carry the R/W bit in bit 0.
	regWriteDmu(2, kI2cDataReg, kI2cIndexWrite | (static_cast<uint32_t>(addr) << kI2cDataShiftI2C));
	for (size_t i = 0; i < wlen; i++)
		regWriteDmu(2, kI2cDataReg, static_cast<uint32_t>(wr[i]) << kI2cDataShiftI2C);
	if (rlen)
		regWriteDmu(2, kI2cDataReg, (addr | 1u) << kI2cDataShiftI2C);

	uint32_t ctl = (static_cast<uint32_t>(line) << kI2cDdcSelectShift) |
	               ((rlen ? 1u : 0u) << kI2cTxnCountShift);  // TRANSACTION_COUNT = transactions - 1
	regWriteDmu(2, kI2cControl, ctl);
	regWriteDmu(2, kI2cControl, ctl | kI2cGo);

	// --- poll for completion. 129 bytes at 100 kHz is ~12 ms; allow 100 ms.
	uint32_t sts = 0;
	bool done = false;
	for (int i = 0; i < 10000; i++) {
		sts = regReadDmu(2, kI2cSwStatus);
		if (sts & (kI2cStoppedOnNack | kI2cSwTimeout | kI2cSwAborted | kI2cSwOverflow))
			break;
		if (sts & kI2cSwDone) { done = true; break; }
		IODelay(10);
	}
	if (!done || (sts & (kI2cStoppedOnNack | kI2cSwTimeout | kI2cSwAborted | kI2cSwOverflow))) {
		FBLOG("i2c: line %u: transaction failed (sw_status=0x%08x%s)", line, sts,
		      (sts & kI2cStoppedOnNack) ? ", NACK" : !done ? ", poll timeout" : "");
		release();
		return false;
	}

	// --- read the reply FIFO (process_channel_reply): the read data starts
	// after the bytes we wrote (address, data, address again).
	if (rlen) {
		regWriteDmu(2, kI2cDataReg,
		            kI2cIndexWrite | kI2cDataRead | (static_cast<uint32_t>(wlen + 2) << kI2cIndexShift));
		for (size_t i = 0; i < rlen; i++)
			rd[i] = static_cast<uint8_t>(
			    (regReadDmu(2, kI2cDataReg) >> kI2cDataShiftI2C) & 0xff);
	}

	release();
	return true;
}

// What amdgpu's write_scdc_data does for a sink that has SCDC: answer its
// version, then say whether the link is scrambled (TMDS_CONFIG).
bool RDNA4Device::scdcConfigure(uint8_t line, uint8_t tmdsConfig) {
	constexpr uint8_t kScdc = 0x54 << 1;                       // HDMI_SCDC_ADDRESS
	constexpr uint8_t kSinkVersion = 0x01, kSourceVersion = 0x02, kTmdsConfig = 0x20;
	uint8_t version = 0;
	if (i2cTransfer(line, kScdc, &kSinkVersion, 1, &version, 1) && version == 1) {
		const uint8_t source[2] = { kSourceVersion, 1 };
		i2cTransfer(line, kScdc, source, sizeof(source), nullptr, 0);
	}
	const uint8_t config[2] = { kTmdsConfig, tmdsConfig };
	const bool ok = i2cTransfer(line, kScdc, config, sizeof(config), nullptr, 0);
	FBLOG("scdc: DDC%u: sink version %u, TMDS_CONFIG = %u %s", line, version, tmdsConfig,
	      ok ? "written" : "NOT written");
	return ok;
}

/*
 * TMDS_CONFIG, written until the sink answers: a display waking from sleep
 * may take a while to (the TV did not answer at the wake's first write and
 * stayed dark on a scrambled link it was not told about, 2026-11-04). A
 * sink that does not answer within 2 s is left as amdgpu leaves it.
 */
bool RDNA4Device::scdcConfigureWait(uint8_t line, uint8_t tmdsConfig)
{
	constexpr int kTries = 20;

	for (int i = 0; i < kTries; i++) {
		if (scdcConfigure(line, tmdsConfig))
			return true;

		if (!tmdsConfig)
			return false;

		IOSleep(100);
	}

	return false;
}

void RDNA4Device::dumpModeState() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("rdna4-modedump", &on, sizeof(on)) || on == 0)
		return;
	if (!ipDiscovery.isValid() || !rmmio)
		return;

	// dcn_4_1_0_offset.h. OTG stride 0x80, DIG stride 0x124, HUBPREQ stride
	// 0xdc, all base_idx 2; DCCG per-OTG pixel-rate regs are base_idx 1.
	FBLOG("mode: --- mode-setting register survey (read-only) ---");
	for (uint32_t i = 0; i < 4; i++) {
		uint32_t o = i * 0x80;
		FBLOG("mode: OTG%u ctl=0x%08x master_en=0x%08x h_total=0x%08x "
		      "h_blank=0x%08x h_sync=0x%08x v_total=0x%08x v_blank=0x%08x "
		      "v_sync=0x%08x", i,
		      regReadDmu(2, 0x1b43 + o), regReadDmu(2, 0x1b5d + o),
		      regReadDmu(2, 0x1b2a + o), regReadDmu(2, 0x1b2b + o),
		      regReadDmu(2, 0x1b2c + o), regReadDmu(2, 0x1b2f + o),
		      regReadDmu(2, 0x1b38 + o), regReadDmu(2, 0x1b39 + o));
	}
	for (uint32_t i = 0; i < 4; i++) {
		uint32_t d = i * 0x124;
		FBLOG("mode: DIG%u fe_cntl=0x%08x fe_clk=0x%08x fe_en=0x%08x "
		      "hdmi_ctl=0x%08x be_cntl=0x%08x be_clk=0x%08x be_en=0x%08x "
		      "tmds=0x%08x", i,
		      regReadDmu(2, 0x2093 + d), regReadDmu(2, 0x2094 + d),
		      regReadDmu(2, 0x2095 + d), regReadDmu(2, 0x209e + d),
		      regReadDmu(2, 0x20bc + d), regReadDmu(2, 0x20bb + d),
		      regReadDmu(2, 0x20bd + d), regReadDmu(2, 0x20e4 + d));
	}
	FBLOG("mode: DCCG dp_dto phase/modulo = [0x%08x/0x%08x 0x%08x/0x%08x "
	      "0x%08x/0x%08x 0x%08x/0x%08x]",
	      regReadDmu(1, 0x0081), regReadDmu(1, 0x0082),
	      regReadDmu(1, 0x0085), regReadDmu(1, 0x0086),
	      regReadDmu(1, 0x0089), regReadDmu(1, 0x008a),
	      regReadDmu(1, 0x008d), regReadDmu(1, 0x008e));
	for (uint32_t i = 0; i < 4; i++) {
		uint32_t h = i * 0xdc;
		FBLOG("mode: HUBP%u pitch=0x%08x addr=0x%08x addr_hi=0x%08x", i,
		      regReadDmu(2, 0x0607 + h), regReadDmu(2, 0x060a + h),
		      regReadDmu(2, 0x060b + h));
	}
	FBLOG("mode: DCCG otg_pixel_rate=[0x%08x 0x%08x 0x%08x 0x%08x]",
	      regReadDmu(1, 0x0080), regReadDmu(1, 0x0084),
	      regReadDmu(1, 0x0088), regReadDmu(1, 0x008c));
	FBLOG("mode: DCCG dtbclk_p=0x%08x symclk32_se=0x%08x dpstreamclk=0x%08x "
	      "phyasymclk=0x%08x gate_disable=0x%08x",
	      regReadDmu(1, 0x0068), regReadDmu(1, 0x0065),
	      regReadDmu(1, 0x004a), regReadDmu(1, 0x0052),
	      regReadDmu(1, 0x0074));
	FBLOG("mode: hdmicharclk=[0x%08x 0x%08x 0x%08x 0x%08x]",
	      regReadDmu(2, 0x004a), regReadDmu(2, 0x004b),
	      regReadDmu(2, 0x004c), regReadDmu(2, 0x004d));
	// DMUB display microcontroller: the VBIOS ships no display command
	// tables (verified via make test), so PHY/PLL work on DCN4 goes through
	// DMUB mailbox commands — IF a firmware is loaded and running. Nonzero
	// CNTL enable / inbox base+pointers / scratch means the GOP left DMUB
	// alive and route (a) is viable; all-zero means route (b) (register-level
	// PHY bring-up from lit-pipe diffs).
	FBLOG("mode: DMCUB cntl=0x%08x cntl2=0x%08x inbox1 base=0x%08x size=0x%08x "
	      "wptr=0x%08x rptr=0x%08x scratch0=0x%08x",
	      regReadDmu(2, 0x01f6), regReadDmu(2, 0x0200),
	      regReadDmu(2, 0x01d4), regReadDmu(2, 0x01d5),
	      regReadDmu(2, 0x01d6), regReadDmu(2, 0x01d7),
	      regReadDmu(2, 0x01e3));
	FBLOG("mode: --- end survey ---");
}

// ---------------------------------------------------------------------------
// Boot pipe discovery
// ---------------------------------------------------------------------------

uint32_t RDNA4Device::pipeRead(void *ctx, uint8_t baseIdx, uint32_t dword) {
	auto *self = static_cast<RDNA4Device *>(ctx);
	return self->regReadDmu(baseIdx, dword);
}

uint64_t RDNA4Device::liveFramePeriodNs() const {
	const uint32_t refresh = liveTimingValid ? liveTiming.refreshMilliHz() : 0;
	return refresh ? 1000000000000ULL / refresh : 0;
}

// Time N frames of the live OTG's frame counter. The TMDS pixel clock sits in
// the PHY PLL where no register exposes it, so frame period x totals is the
// only way to learn the exact rate the GOP chose. Edges are detected by
// polling every 10 us, so 10 frames give ~0.01 % precision. Returns 0 if the
// counter does not move (OTG stopped / unreadable).
uint64_t RDNA4Device::measureFramePeriodNs(uint32_t otgOffset) {
	if (!pipe.valid())
		return 0;
	constexpr uint32_t kFrames = 10;
	const uint32_t reg = Pipe::Reg::kOtgFrameCount + otgOffset;

	auto count = [&]() { return regReadDmu(2, reg) & 0xffffff; };
	auto nowNs = [&]() {
		uint64_t abs = 0, ns = 0;
		clock_get_uptime(&abs);
		absolutetime_to_nanoseconds(abs, &ns);
		return ns;
	};
	// Wait for a frame edge, at most ~100 ms.
	auto waitEdge = [&](uint32_t from, uint32_t &to) -> bool {
		for (int i = 0; i < 10000; i++) {
			uint32_t c = count();
			if (c != from) {
				to = c;
				return true;
			}
			IODelay(10);
		}
		return false;
	};

	uint32_t c0 = count(), start = 0;
	if (regReadDmu(2, reg) == 0xFFFFFFFF || !waitEdge(c0, start))
		return 0;
	uint64_t t0 = nowNs();
	uint32_t c = start;
	for (uint32_t n = 0; n < kFrames; n++) {
		uint32_t next = 0;
		if (!waitEdge(c, next))
			return 0;
		c = next;
	}
	uint64_t t1 = nowNs();
	uint32_t frames = (c - start) & 0xffffff;
	return frames ? (t1 - t0) / frames : 0;
}

void RDNA4Device::discoverPipe() {
	if (!ipDiscovery.isValid() || !rmmio)
		return;
	if (!Pipe::discover(&RDNA4Device::pipeRead, this, pipe)) {
		FBLOG("pipe: no running OTG found — keeping upstream pipe-0 assumptions");
		return;
	}
	FBLOG("pipe: lit pipe OTG%u DIG%u -> link%u (HPD%u) OPP%u HUBP%u, signal %s",
	      pipe.otg, pipe.dig, pipe.link, pipe.hpd, pipe.opp, pipe.hubp,
	      Pipe::signalName(pipe.signal));
	FBLOG("pipe: OTG h_total=0x%08x h_blank=0x%08x h_sync=0x%08x v_total=0x%08x "
	      "v_blank=0x%08x v_sync=0x%08x pol h%c v%c; HUBP viewport %ux%u pitch %u px",
	      pipe.hTotal, pipe.hBlank, pipe.hSync, pipe.vTotal, pipe.vBlank, pipe.vSync,
	      pipe.hSyncNeg ? '-' : '+', pipe.vSyncNeg ? '-' : '+',
	      pipe.viewportW, pipe.viewportH, pipe.pitchPx);

	owner->setProperty("Pipe,OTG", static_cast<uint64_t>(pipe.otg), 32);
	owner->setProperty("Pipe,DIG", static_cast<uint64_t>(pipe.dig), 32);
	owner->setProperty("Pipe,Link", static_cast<uint64_t>(pipe.link), 32);
	owner->setProperty("Pipe,HPD", static_cast<uint64_t>(pipe.hpd), 32);
	owner->setProperty("Pipe,HUBP", static_cast<uint64_t>(pipe.hubp), 32);
	owner->setProperty("Pipe,Signal", Pipe::signalName(pipe.signal));

	bootTimingValid = Pipe::timingFromOtg(pipe, bootTiming);
	if (!bootTimingValid) {
		FBLOG("pipe: OTG timing did not decode");
		return;
	}
	uint64_t periodNs = measureFramePeriodNs();
	bootTiming.pixelClockKHz = Pipe::pixelClockFromFramePeriod(pipe, periodNs);
	FBLOG("pipe: boot timing %ux%u, h %u/%u/%u v %u/%u/%u, frame %llu ns -> "
	      "pixel clock ~%u kHz (%u.%03u Hz)",
	      bootTiming.hActive, bootTiming.vActive,
	      bootTiming.hSyncOffset, bootTiming.hSyncWidth, bootTiming.hBlank,
	      bootTiming.vSyncOffset, bootTiming.vSyncWidth, bootTiming.vBlank,
	      periodNs, bootTiming.pixelClockKHz,
	      bootTiming.refreshMilliHz() / 1000, bootTiming.refreshMilliHz() % 1000);
	char mode[40];
	snprintf(mode, sizeof(mode), "%ux%u@%u.%03u", bootTiming.hActive, bootTiming.vActive,
	         bootTiming.refreshMilliHz() / 1000, bootTiming.refreshMilliHz() % 1000);
	owner->setProperty("Pipe,BootMode", mode);
	owner->setProperty("Pipe,MeasuredPixelClockKHz", static_cast<uint64_t>(bootTiming.pixelClockKHz), 32);
}

// ---------------------------------------------------------------------------
// DMUB mailbox (display firmware)
// ---------------------------------------------------------------------------

// Indirect VRAM access through MM_INDEX/MM_DATA (BIF_BX_PF0, BAR5 bytes
// 0x0 / 0x4 / 0x18) — amdgpu_device_mm_access. Reaches any VRAM byte
// through the register BAR; the way to touch the DMUB ring in top-of-VRAM
// firmware memory that the 256 MiB CPU aperture cannot map.
uint32_t RDNA4Device::vramRead32(uint64_t pos) {
	if (!rmmio || rmmioSize < 0x1c)
		return 0xFFFFFFFF;
	rmmio[0x18 / 4] = static_cast<uint32_t>(pos >> 31);
	rmmio[0]        = (static_cast<uint32_t>(pos) & 0x7ffffffc) | 0x80000000u;
	return rmmio[1];
}

void RDNA4Device::vramWrite32(uint64_t pos, uint32_t value) {
	if (!rmmio || rmmioSize < 0x1c)
		return;
	rmmio[0x18 / 4] = static_cast<uint32_t>(pos >> 31);
	rmmio[0]        = (static_cast<uint32_t>(pos) & 0x7ffffffc) | 0x80000000u;
	rmmio[1]        = value;
}

namespace {
// DMCUB mailbox registers (dcn_4_1_0_offset.h, base_idx 2).
constexpr uint32_t kDmcubRegion4Offset     = 0x0196;   // mailbox MC address, low
constexpr uint32_t kDmcubRegion4OffsetHigh = 0x0197;
constexpr uint32_t kDmcubCw3Base           = 0x01a8;   // DMCUB_REGION3_CW3..6_BASE_ADDRESS (the firmware's view)
constexpr uint32_t kDmcubCw3Top            = 0x01b0;   // DMCUB_REGION3_CW3..6_TOP_ADDRESS (bit 31: enabled)
constexpr uint32_t kDmcubCw3Offset         = 0x01bb;   // DMCUB_REGION3_CW3..6_OFFSET, _HIGH: their MC addresses
constexpr uint32_t kDmcubInbox1Base        = 0x01d4;
constexpr uint32_t kDmcubInbox1Size        = 0x01d5;
constexpr uint32_t kDmcubInbox1Wptr        = 0x01d6;
constexpr uint32_t kDmcubInbox1Rptr        = 0x01d7;
constexpr uint32_t kDcnVmFbLocationBase    = 0x0475;   // VRAM MC base, 16 MiB units
} // namespace

bool RDNA4Device::dmubRing(DmubRing &ring) {
	ring = DmubRing {};
	if (!ipDiscovery.isValid() || !rmmio)
		return false;
	// Inbox 1 is where the firmware sees it (DMCUB_INBOX1_BASE_ADDRESS), in the region that holds that address:
	// REGION4 from 0x80000000 (the GOP's firmware: the inbox at 0x80000000, REGION4 at its MC address), else the
	// window 3-6 whose base..top holds it (Linux's start-up, the kext's own firmware with rdna4-dmubload: the
	// inbox at the start of window 4, REGION4 left as the GOP had it). Bases and tops read back as 29 bits.
	const uint32_t inbox = regReadDmu(2, kDmcubInbox1Base);
	uint64_t inboxMc = 0, windowMc = 0;
	if (inbox >= 0x80000000u) {
		windowMc = regReadDmu(2, kDmcubRegion4Offset) |
		    (static_cast<uint64_t>(regReadDmu(2, kDmcubRegion4OffsetHigh)) << 32);
		inboxMc = windowMc + (inbox - 0x80000000u);
	} else {
		const uint32_t at = inbox & 0x1fffffff;
		for (uint32_t cw = 3; cw <= 6 && !inboxMc; cw++) {
			const uint32_t base = regReadDmu(2, kDmcubCw3Base + cw - 3) & 0x1fffffff;
			const uint32_t top = regReadDmu(2, kDmcubCw3Top + cw - 3);
			if (!(top & 0x80000000u) || at < base || at >= (top & 0x1fffffff))
				continue;
			const uint32_t off = kDmcubCw3Offset + 2 * (cw - 3);
			windowMc = regReadDmu(2, off) | (static_cast<uint64_t>(regReadDmu(2, off + 1)) << 32);
			inboxMc = windowMc + (at - base);
		}
	}
	uint64_t vramMcBase =
	    static_cast<uint64_t>(regReadDmu(2, kDcnVmFbLocationBase) & 0xffffff) << 24;
	if (inboxMc <= vramMcBase) {
		FBLOG("dmub: inbox 0x%08x in no region or not above vram base 0x%llx (at MC 0x%llx)", inbox, vramMcBase,
		      inboxMc);
		return false;
	}
	uint32_t size = regReadDmu(2, kDmcubInbox1Size);
	if (size == 0 || size > 0x100000 || (size % Dmub::kCmdSize) != 0) {
		FBLOG("dmub: inbox1 size 0x%x not sane", size);
		return false;
	}
	ring.base = inboxMc - vramMcBase;
	ring.size = size;
	static uint64_t logged;
	if (logged != inboxMc) {   // once per firmware: the GOP's, then the kext's own (rdna4-dmubload)
		logged = inboxMc;
		FBLOG("dmub: inbox 1 (0x%08x) at MC 0x%llx, its region at 0x%llx, 0x%x bytes", inbox, inboxMc, windowMc, size);
	}
	return true;
}

bool RDNA4Device::dmubSubmit(const Dmub::Cmd *cmds, uint32_t count, const char *tag,
                         uint64_t *firstSlot) {
	DmubRing ring;
	if (!cmds || count == 0 || !dmubRing(ring))
		return false;
	if (count * Dmub::kCmdSize >= ring.size) {
		FBLOG("%s: %u commands do not fit the 0x%x-byte inbox", tag, count, ring.size);
		return false;
	}
	// The GOP firmware services the inbox1 ring (it ignores register inbox0);
	// only submit into an idle ring so we never race a firmware-side reader.
	uint32_t wptr = regReadDmu(2, kDmcubInbox1Wptr);
	uint32_t rptr = regReadDmu(2, kDmcubInbox1Rptr);
	if (wptr != rptr || wptr >= ring.size || (wptr % Dmub::kCmdSize) != 0) {
		FBLOG("%s: inbox not idle/sane (size=0x%x wptr=0x%x rptr=0x%x)",
		      tag, ring.size, wptr, rptr);
		return false;
	}
	if (firstSlot)
		*firstSlot = ring.base + wptr;

	uint32_t slot = wptr;
	for (uint32_t c = 0; c < count; c++) {
		uint64_t pos = ring.base + slot;
		for (uint32_t i = 0; i < Dmub::kCmdSize / 4; i++)
			vramWrite32(pos + 4 * i, cmds[c][i]);
		// Proof the MM window writes the memory the firmware reads.
		uint32_t rb = vramRead32(pos);
		if (rb != cmds[c][0]) {
			FBLOG("%s: ring write readback mismatch at +0x%x (0x%08x != 0x%08x), "
			      "nothing submitted", tag, slot, rb, cmds[c][0]);
			return false;
		}
		slot = (slot + Dmub::kCmdSize) % ring.size;
	}

	regWriteDmu(2, kDmcubInbox1Wptr, slot);
	uint32_t nrptr = rptr;
	for (int i = 0; i < 20000; i++) {          // 200 ms budget
		nrptr = regReadDmu(2, kDmcubInbox1Rptr);
		if (nrptr == slot) {
			dmubTrace(tag);
			return true;
		}
		IODelay(10);
	}
	FBLOG("%s: firmware did not consume (wptr=0x%x rptr stuck at 0x%x)", tag, slot, nrptr);
	dmubTrace(tag);
	return false;
}

// The trace buffer (window 5, Linux's dmub_tracebuffer_show): a 16-byte header whose first dword counts the
// entries ever written, then a ring of 16-byte entries {trace_code, tick, param0, param1}. Only with
// rdna4-dmubload, where the window is the kext's (the GOP's firmware has its own layout).
void RDNA4Device::dmubTrace(const char *tag) {
	static int on = -1;
	if (on < 0) {
		uint32_t v = 0;
		on = Trial::bootArg("rdna4-dmubload", v) && v;
	}
	const uint32_t cw5 = 5 - 3;   // window 5's index in the CW3..6 register runs
	const uint64_t mc = regReadDmu(2, kDmcubCw3Offset + 2 * cw5) |
	    static_cast<uint64_t>(regReadDmu(2, kDmcubCw3Offset + 2 * cw5 + 1)) << 32;
	const uint64_t vramMcBase = static_cast<uint64_t>(regReadDmu(2, kDcnVmFbLocationBase) & 0xffffff) << 24;
	const uint32_t size = (regReadDmu(2, kDmcubCw3Top + cw5) & 0x1fffffff) - (regReadDmu(2, kDmcubCw3Base + cw5) & 0x1fffffff);
	if (!on || mc <= vramMcBase || size < 32 || size > 0x100000)
		return;
	const uint64_t at = mc - vramMcBase;
	const uint32_t count = vramRead32(at), ring = (size - 16) / 16;
	if (count < dmubTraceSeen)   // another firmware's buffer: its count starts again
		dmubTraceSeen = 0;
	const uint32_t first = count - dmubTraceSeen > 32 ? count - 32 : dmubTraceSeen;
	FBLOG("%s: dmub trace: %u entries (%u new)", tag, count, count - dmubTraceSeen);
	for (uint32_t n = first; n < count; n++) {
		const uint64_t e = at + 16 + 16ull * (n % ring);
		FBLOG("%s: dmub trace %u: code 0x%x tick %u params 0x%08x 0x%08x", tag, n, vramRead32(e), vramRead32(e + 4),
		      vramRead32(e + 8), vramRead32(e + 12));
	}
	dmubTraceSeen = count;
}

// rdna4-smuping=1: first contact with the SMU power-management firmware
// (MP1 mailbox, smu_v14_0_send_msg_with_param protocol; offsets from
// mp_14_0_2_offset.h, port informed by lemonade-sdk/mac-amdgpu, MIT).
// Read-only: TestMessage plus two version queries — no DPM state is
// touched, so the live display is unaffected. A responding SMU is the
// prerequisite for the clocks/power roadmap item.
// rdna4-ihdump=1: read-only survey of the interrupt delivery landscape —
// the prerequisite map for replacing the timer-emulated VBL with real
// vertical-blank interrupts (IH v7 ring + MSI, as brought up on this die
// by lemonade-sdk/mac-amdgpu). Dumps the OSSSYS IH ring state (did the
// GOP leave an interrupt ring configured? — usually not), the per-OTG
// vertical-interrupt line config on the DCN side, and the PCI MSI/MSI-X
// capability state. Writes nothing.
// rdna4-pspdump=1: read-only survey of the PSP (security processor) —
// the gatekeeper for loading fresh firmware (a current dmub_dcn401.bin
// would replace the limited GOP DMUB). Registers are the MPASP scratch
// block (mp_14_0_2_offset.h, MP0 base_idx 0), semantics per upstream
// psp_v14_0.c and lemonade-sdk/mac-amdgpu (MIT), which drove the full
// LOAD_IP_FW chain on this die from macOS. Nothing is written: this only
// answers "is the GOP-posted PSP alive and command-able?".
void RDNA4Device::dumpPSP() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("rdna4-pspdump", &on, sizeof(on)) || on == 0)
		return;
	if (!ipDiscovery.isValid() || !rmmio)
		return;

	auto psp = [&](uint32_t dword) -> uint32_t {
		uint32_t off;
		if (!ipDiscovery.regByteOffset(IpDiscovery::HwMp0, 0, 0, dword, off) ||
		    off + 4 > rmmioSize)
			return 0xFFFFFFFF;
		return rmmio[off / 4];
	};

	uint32_t boot = psp(0x0063);   // C2PMSG_35: bit31 = bootloader ready
	uint32_t sol  = psp(0x0091);   // C2PMSG_81: nonzero = sOS alive (build stamp)
	uint32_t ring = psp(0x0080);   // C2PMSG_64: GPCOM ring status
	FBLOG("psp: boot(C2PMSG_35)=0x%08x sos(C2PMSG_81)=0x%08x ring(C2PMSG_64)=0x%08x",
	      boot, sol, ring);
	FBLOG("psp: ring regs 69/70/71 = 0x%08x 0x%08x 0x%08x, fw ver 58/59 = "
	      "0x%08x 0x%08x",
	      psp(0x0085), psp(0x0086), psp(0x0087), psp(0x007a), psp(0x007b));
	FBLOG("psp: verdict: bootloader %s, sOS %s, GPCOM ring %s",
	      (boot & 0x80000000u) ? "READY" : "not ready",
	      sol ? "ALIVE" : "not running",
	      (ring & 0x80000000u) ? "configured" : "not configured");
	if (sol) {
		owner->setProperty("PSP,SOSVersion", static_cast<uint64_t>(sol), 32);
		owner->setProperty("PSP,Alive", true);
	}
}

void RDNA4Device::dumpIH() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("rdna4-ihdump", &on, sizeof(on)) || on == 0)
		return;
	if (!ipDiscovery.isValid() || !rmmio)
		return;

	// OSSSYS IH ring registers (osssys_7_0_0_offset.h, base_idx 0).
	auto ihRead = [&](uint32_t dword) -> uint32_t {
		uint32_t off;
		if (!ipDiscovery.regByteOffset(IpDiscovery::HwOsssys, 0, 0, dword, off) ||
		    off + 4 > rmmioSize)
			return 0xFFFFFFFF;
		return rmmio[off / 4];
	};
	FBLOG("ih: RB cntl=0x%08x base=0x%08x/%08x rptr=0x%08x wptr=0x%08x "
	      "doorbell=0x%08x cntl2=0x%08x status=0x%08x",
	      ihRead(0x0080), ihRead(0x0083), ihRead(0x0084),
	      ihRead(0x0081), ihRead(0x0082), ihRead(0x0087),
	      ihRead(0x00a8), ihRead(0x00c2));

	// DCN-side vertical interrupt lines for the live OTG: three independent
	// lines, each with a raster position trigger and an enable. amdgpu uses
	// line 0 for VUPDATE-ish events, line 2 for the VBLANK the DRM core
	// consumes. All-zero controls mean the GOP never enabled any of them.
	const uint32_t o = otgOff();
	FBLOG("ih: OTG%u vline0 pos=0x%08x ctl=0x%08x  vline1 pos=0x%08x ctl=0x%08x  "
	      "vline2 pos=0x%08x ctl=0x%08x", pipe.otg < Pipe::kMaxOtg ? pipe.otg : 0,
	      regReadDmu(2, 0x1b5f + o), regReadDmu(2, 0x1b60 + o),
	      regReadDmu(2, 0x1b61 + o), regReadDmu(2, 0x1b62 + o),
	      regReadDmu(2, 0x1b63 + o), regReadDmu(2, 0x1b64 + o));

	// PCI interrupt capability state: MSI (cap 0x05) and MSI-X (cap 0x11).
	// A kext receives these via IOInterruptEventSource once enabled; this
	// records what the config space offers before we ever touch it.
	if (pciDevice) {
		IOByteCount capMsi = 0, capMsix = 0;
		uint32_t msiCtl = 0, msixCtl = 0;
		if (pciDevice->extendedFindPCICapability(0x05, &capMsi) && capMsi)
			msiCtl = pciDevice->configRead32(static_cast<uint8_t>(capMsi));
		if (pciDevice->extendedFindPCICapability(0x11, &capMsix) && capMsix)
			msixCtl = pciDevice->configRead32(static_cast<uint8_t>(capMsix));
		FBLOG("ih: pci msi cap@0x%x ctl=0x%08x, msi-x cap@0x%x ctl=0x%08x",
		      (uint32_t)capMsi, msiCtl, (uint32_t)capMsix, msixCtl);
	}
}

void RDNA4Device::smuPing() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("rdna4-smuping", &on, sizeof(on)) || on == 0)
		return;
	if (!ipDiscovery.isValid() || !rmmio) {
		FBLOG("smu: MMIO/discovery unavailable, skipping ping");
		return;
	}

	// regMP1_SMN_C2PMSG_66/82/90, all BASE_IDX 1 (base 0 routes the writes
	// to a different physical register and the SMU never answers).
	uint32_t regMsg, regParam, regResp;
	if (!ipDiscovery.regByteOffset(IpDiscovery::HwMp1, 0, 1, 0x0082, regMsg) ||
	    !ipDiscovery.regByteOffset(IpDiscovery::HwMp1, 0, 1, 0x0092, regParam) ||
	    !ipDiscovery.regByteOffset(IpDiscovery::HwMp1, 0, 1, 0x009a, regResp)) {
		FBLOG("smu: MP1 base_idx 1 not in discovery table");
		return;
	}
	FBLOG("smu: mailbox msg@0x%x param@0x%x resp@0x%x", regMsg, regParam, regResp);
	if (regMsg + 4 > rmmioSize || regParam + 4 > rmmioSize || regResp + 4 > rmmioSize) {
		FBLOG("smu: mailbox outside BAR5 aperture");
		return;
	}

	// One message exchange: clear resp, stage param, kick, poll resp.
	// Response codes: 1 OK, 0xFF failed, 0xFE unknown cmd, 0xFD prereq,
	// 0xFC busy.
	auto send = [&](uint32_t msgId, uint32_t param, uint32_t *ret) -> uint32_t {
		rmmio[regResp / 4]  = 0;
		rmmio[regParam / 4] = param;
		rmmio[regMsg / 4]   = msgId;
		uint32_t resp = 0;
		for (int i = 0; i < 500; i++) {         // 500 ms budget
			resp = rmmio[regResp / 4];
			if (resp != 0)
				break;
			IOSleep(1);
		}
		if (ret)
			*ret = rmmio[regParam / 4];
		return resp;
	};

	// PPSMC message ids (smu_v14_0_2_ppsmc.h): TestMessage=1,
	// GetSmuVersion=2, GetDriverIfVersion=3.
	uint32_t ret = 0;
	uint32_t resp = send(0x1, 0xC0FFEE, &ret);
	if (resp != 1) {
		FBLOG("smu: TestMessage resp=0x%02x — PMFW not answering (ret=0x%08x)",
		      resp, ret);
		return;
	}
	FBLOG("smu: PING OK — TestMessage acked (ret=0x%08x)", ret);

	if (send(0x2, 0, &ret) == 1) {
		FBLOG("smu: PMFW version 0x%08x (%u.%u.%u)", ret,
		      (ret >> 16) & 0xff, (ret >> 8) & 0xff, ret & 0xff);
		owner->setProperty("SMU,FirmwareVersion", static_cast<uint64_t>(ret), 32);
	}
	if (send(0x3, 0, &ret) == 1) {
		FBLOG("smu: driver interface version 0x%08x", ret);
		owner->setProperty("SMU,DriverIfVersion", static_cast<uint64_t>(ret), 32);
	}
	owner->setProperty("SMU,Verified", true);
}

// rdna4-dmubhist=1: read-only decode of the GOP's own DMUB command history.
// The inbox1 ring still holds every command the GOP issued to bring up the
// DP0 display (wptr bytes worth, 64 each), and the GOP does display bring-up
// entirely through this ring (VBIOS-family commands). Decoding them yields
// the exact, firmware-accepted mode-set recipe for THIS silicon — the
// template to adapt for the HDMI pipe. Pure MM_INDEX reads; writes nothing.
void RDNA4Device::dmubHistory() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("rdna4-dmubhist", &on, sizeof(on)) || on == 0)
		return;
	if (!ipDiscovery.isValid() || !rmmio)
		return;

	uint32_t wptr = regReadDmu(2, kDmcubInbox1Wptr);
	if (wptr == 0 || wptr > 0x2000 || (wptr % Dmub::kCmdSize) != 0) {
		FBLOG("dmub-hist: wptr 0x%x not a sane command count", wptr);
		return;
	}
	DmubRing ring;
	if (!dmubRing(ring))
		return;
	uint64_t ringBase = ring.base;
	uint32_t count = wptr / Dmub::kCmdSize;
	FBLOG("dmub-hist: decoding %u GOP commands from vram+0x%llx", count, ringBase);

	for (uint32_t c = 0; c < count && c < 40; c++) {
		uint64_t slot = ringBase + static_cast<uint64_t>(c) * Dmub::kCmdSize;
		uint32_t hdr = vramRead32(slot);
		uint8_t  type    = hdr & 0xff;
		uint8_t  subType = (hdr >> 8) & 0xff;
		uint8_t  payload = (hdr >> 24) & 0x3f;
		uint32_t d1 = vramRead32(slot + 4),  d2 = vramRead32(slot + 8);
		uint32_t d3 = vramRead32(slot + 12), d4 = vramRead32(slot + 16);
		uint32_t d5 = vramRead32(slot + 20), d6 = vramRead32(slot + 24);
		FBLOG("dmub-hist: [%02u] hdr=0x%08x %-30s pl=%u  %08x %08x %08x %08x %08x %08x",
		      c, hdr, Dmub::cmdLabel(type, subType), payload,
		      d1, d2, d3, d4, d5, d6);
	}
	FBLOG("dmub-hist: --- end (this is the DP0 bring-up recipe to adapt for HDMI) ---");
}

// rdna4-dmubver=1: read-only fingerprint of the GOP-loaded DMUB firmware so
// its command dialect can be matched against the amdgpu-loaded DMUB on the
// Debian dual-boot. amdgpu's captured HDMI modeset uses mainline VBIOS
// subtypes (0 encoder / 1 transmitter / 2 pixel-clock), but that is a
// different firmware blob than the GOP's — whose own DP0 bring-up spoke
// subtypes 6/10/12/16. If the two firmware versions match, the mainline
// recipe is safe to replay against this DMUB; if not, the GOP has its own
// dialect and the capture is reference-only. DMUB surfaces build/version/
// boot state in the DMCUB_SCRATCH bank (SCRATCH0 = boot status; higher
// slots carry version/build markers on most builds). Pure register reads —
// no ring traffic, no state change, safe alongside the live DP0 console.
// rdna4-dmubver=2 adds one write: the GPINT version query below.
void RDNA4Device::dumpDmubVersion() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("rdna4-dmubver", &on, sizeof(on)) || on == 0)
		return;
	if (!rmmio)
		return;

	// DMCUB_SCRATCH0 is DMU dword 0x01e3; the bank runs consecutively up to
	// DMCUB_CNTL at 0x01f6 (SCRATCH0..SCRATCH18).
	constexpr uint32_t kScratch0 = 0x01e3;
	for (uint32_t i = 0; i < 18; i += 6) {
		FBLOG("dmubver: SCRATCH%02u-%02u = 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x",
		      i, i + 5,
		      regReadDmu(2, kScratch0 + i + 0), regReadDmu(2, kScratch0 + i + 1),
		      regReadDmu(2, kScratch0 + i + 2), regReadDmu(2, kScratch0 + i + 3),
		      regReadDmu(2, kScratch0 + i + 4), regReadDmu(2, kScratch0 + i + 5));
	}
	// Enable/boot context plus region4 mc (the ring lives here) — the anchor
	// for the fw-meta read we escalate to if the scratch bank is inconclusive.
	uint64_t region4Mc = regReadDmu(2, kDmcubRegion4Offset) |
	    (static_cast<uint64_t>(regReadDmu(2, kDmcubRegion4OffsetHigh)) << 32);
	FBLOG("dmubver: cntl=0x%08x cntl2=0x%08x region4 mc=0x%llx",
	      regReadDmu(2, 0x01f6), regReadDmu(2, 0x0200), region4Mc);

	// rdna4-dmubver=2: ask the firmware itself (DMUB_GPINT__GET_FW_VERSION).
	// This is the one write here: DMCUB_GPINT_DATAIN1, which interrupts the
	// firmware. Level 1 stays read-only. Only sent to an enabled DMCUB whose
	// boot status says the mailbox is ready.
	if (on >= 2) {
		constexpr uint32_t kGpintDataIn1 = 0x01f8, kScratch7 = kScratch0 + 7;
		const uint32_t cntl = regReadDmu(2, 0x01f6), boot = regReadDmu(2, kScratch0);
		const uint32_t word = Dmub::gpintWord(Dmub::GpintGetFwVersion, 0);
		if (cntl == 0xFFFFFFFF || !(cntl & (1u << 16)) || !(boot & 0x2)) {   // DMCUB_ENABLE, mailbox_rdy
			FBLOG("dmubver: GPINT not sent: DMCUB not enabled or its mailbox not ready");
		} else {
			const uint32_t before = regReadDmu(2, kScratch7);
			regWriteDmu(2, kGpintDataIn1, word);
			const uint32_t acked = Dmub::gpintAcked(word);
			uint32_t seen = regReadDmu(2, kGpintDataIn1), us = 0;
			while (seen != acked && us < 100000) {
				IODelay(10);
				us += 10;
				seen = regReadDmu(2, kGpintDataIn1);
			}
			if (seen == acked)
				FBLOG("dmubver: GPINT GET_FW_VERSION acknowledged after %u us: fw_version=0x%08x "
				      "(SCRATCH7 was 0x%08x); amdgpu's own DMUB on Debian reports 0x00010300",
				      us, regReadDmu(2, kScratch7), before);
			else
				FBLOG("dmubver: GPINT GET_FW_VERSION not acknowledged in 100 ms (DATAIN1 reads "
				      "0x%08x): this firmware does not serve GPINT", seen);
		}
	}

	// The scratch bank carries boot status, not the firmware version, so it
	// can't answer the dialect question on its own. Escalate: dmub_fw_meta_info
	// is embedded at the tail of the loaded firmware image (magic 0x444D5542
	// "DMUB", then fw_region_size, trace_buffer_size, fw_version). Scan VRAM
	// downward from the region4 (mailbox) anchor for the magic and decode the
	// real fw_version — the apples-to-apples value to diff against Debian's
	// `version=0x...`. Pure MM_INDEX reads, bounded, writes nothing.
	DmubRing ring;
	if (!dmubRing(ring)) {
		FBLOG("dmubver: no usable region4 — skipping fw-meta scan");
		return;
	}
	uint64_t region4Off = ring.base;
	constexpr uint32_t kMetaMagic = 0x444d5542;   // "DMUB"
	constexpr uint64_t kScanBytes = 0x100000;     // 1 MiB below the mailbox
	uint64_t found = 0;
	for (uint64_t off = 4; off <= kScanBytes && off <= region4Off; off += 4) {
		if (vramRead32(region4Off - off) == kMetaMagic) {
			found = region4Off - off;
			break;
		}
	}
	if (!found) {
		FBLOG("dmubver: fw-meta magic not found in 1MiB below region4 "
		      "(widen scan or dump the CW region map)");
		return;
	}
	uint32_t fwRegion = vramRead32(found + 4);
	uint32_t traceBuf = vramRead32(found + 8);
	uint32_t fwVersion = vramRead32(found + 12);
	FBLOG("dmubver: fw-meta @ vram+0x%llx fw_version=0x%08x "
	      "region_size=0x%x trace_buf=0x%x",
	      found, fwVersion, fwRegion, traceBuf);
	FBLOG("dmubver: fw_version 0x%08x %s Debian 0x00010300 — match => same "
	      "blob, mainline VBIOS subtypes 0/1/2 safe to replay",
	      fwVersion, fwVersion == 0x00010300 ? "==" : "!=");
}

void RDNA4Device::dmubPing() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("rdna4-dmubping", &on, sizeof(on)) || on == 0)
		return;
	if (!ipDiscovery.isValid() || !rmmio) {
		FBLOG("dmub: MMIO unavailable, skipping ping");
		return;
	}

	DmubRing ring;
	if (!dmubRing(ring))
		return;
	uint32_t wptr = regReadDmu(2, kDmcubInbox1Wptr);
	FBLOG("dmub: inbox1 ring at vram+0x%llx, size=0x%x wptr=0x%x",
	      ring.base, ring.size, wptr);

	// Sanity: the entry before wptr was written by the GOP; read it back as
	// proof the MM window reads the same memory the firmware reads.
	if (wptr >= Dmub::kCmdSize && wptr < ring.size)
		FBLOG("dmub: previous GOP command header via MM: 0x%08x",
		      vramRead32(ring.base + wptr - Dmub::kCmdSize));

	// A harmless QUERY_FEATURE_CAPS: the firmware reports caps, changes nothing.
	Dmub::Cmd cmd;
	Dmub::clear(cmd);
	cmd[0] = Dmub::headerWord(Dmub::CmdQueryFeatureCaps, 0, Dmub::kCmdSize - 4);
	uint64_t slot = 0;
	if (dmubSubmit(&cmd, 1, "dmub", &slot))
		FBLOG("dmub: PING OK — consumed; reply: %08x %08x %08x",
		      vramRead32(slot), vramRead32(slot + 4), vramRead32(slot + 8));
	else
		FBLOG("dmub: ping NOT consumed");
}

void RDNA4Device::displayPowerNote(bool on, const char *path, const char *result) {
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	const uint32_t n = displayPowerEvents++;
	snprintf(displayPowerHist[n % 16], sizeof(displayPowerHist[0]), "#%u t%llums %s %s: %s", n + 1,
	         static_cast<unsigned long long>(ns / 1000000ull), on ? "ON" : "OFF", path, result);
	if (!owner)
		return;
	char all[16 * 76 + 8];
	size_t len = 0;
	const uint32_t count = displayPowerEvents < 16 ? displayPowerEvents : 16;
	for (uint32_t i = 0; i < count; i++) {          // oldest first
		const uint32_t idx = (displayPowerEvents - count + i) % 16;
		len += snprintf(all + len, sizeof(all) - len, "%s%s", i ? " ## " : "", displayPowerHist[idx]);
		if (len >= sizeof(all))
			break;
	}
	owner->setProperty("AMDRadeonRDNA4,DisplayPower", all);
}

void RDNA4Device::setDisplayPower(bool on) {
	if (bootReleased) {   // its connector is a head's now (releaseBootPipe)
		displayPowerNote(on, "request", "ignored: the boot pipe is released");
		return;
	}
	if (!displaySleepEnabled) {
		displayPowerNote(on, "request", "ignored: rdna4-nosleep=1");
		return;
	}
	if (on == displayPowerOn)
		return;
	if (!ipDiscovery.isValid() || !rmmio) {
		displayPowerNote(on, "request", "ignored: no discovery/MMIO");
		return;
	}

	// HDMI/DVI boot pipe, with the mode-set engine asked for (rdna4-modeset=1):
	// the link goes down, so the monitor loses the signal and sleeps, and the
	// way back is the mode set to the running timing (ModeSet::buildSleep).
	if (pipe.valid() && pipe.isTmds() && (on ? hdmiLinkOff : modesetRequested && liveTimingValid && liveTiming.hActive <= fbWidth &&
	                                              liveTiming.vActive <= fbHeight)) {
		ModeSet::Target t {};
		const char *why = "";
		char res[48];
		if (on) {
			const IOReturn ret = applyMode(Modes::Mode { currentModeId, liveTiming, liveTiming.refreshMilliHz(),
			                                             false, Modes::SourceBoot });
			hdmiLinkOff = false;
			FBLOG("power: HDMI display on: mode set to the running timing %s (0x%x)",
			      ret == kIOReturnSuccess ? "done" : "FAILED", ret);
			snprintf(res, sizeof(res), "mode set 0x%x", ret);
			displayPowerNote(true, "HDMI link", res);
			displayPowerOn = true;
			return;
		}
		t.to = liveTiming;
		if (bootPipeTarget(t) && ModeSet::buildSleep(t, modePlan, &why)) {
			const bool ran = runPlan(modePlan);
			hdmiLinkOff = true;   // also after a plan that stopped: waking is a full mode set either way
			const uint32_t fe = regReadDmu(2, Pipe::Reg::kDigFeEnCntl + digOff());
			FBLOG("power: HDMI display off: stream encoder and transmitter down, %lu steps %s; "
			      "DIG%u_DIG_FE_EN_CNTL=0x%08x", static_cast<unsigned long>(modePlan.count),
			      ran ? "ran" : "STOPPED", pipe.dig, fe);
			snprintf(res, sizeof(res), "%s, FE %08x", ran ? "ran" : "STOPPED", fe);
			displayPowerNote(false, "HDMI link", res);
			displayPowerOn = false;
			return;
		}
		FBLOG("power: HDMI link stays up (%s): blanking instead", why[0] ? why : "no VBIOS path");
	}

	// Otherwise an HDMI/DVI boot pipe is blanked: there is no DP stream or
	// DPCD to toggle. Solid black with the OPP's display pattern generator, the way
	// opp2_set_disp_pattern_generator does for SOLID_COLOR (DPG colours 0,
	// DPG_MODE = TEST_PATTERN_MODE_HORIZONTALBARS, DPG_EN).
	if (pipe.valid() && pipe.isTmds()) {
		constexpr uint32_t kDpgControl    = 0x1854;   // DPG_EN [0], DPG_MODE [6:4]
		constexpr uint32_t kDpgDimensions = 0x1856;   // WIDTH [29:16], HEIGHT [13:0]
		constexpr uint32_t kDpgColourRCr  = 0x1857;
		constexpr uint32_t kDpgColourGY   = 0x1858;
		constexpr uint32_t kDpgColourBCb  = 0x1859;
		constexpr uint32_t kDpgModeHorizontalBars = 4;
		const uint32_t o = oppOff();

		ensureUpdateLatch();
		if (!on) {
			dpgSavedControl = regReadDmu(2, kDpgControl + o);
			regWriteDmu(2, kDpgColourRCr + o, 0);
			regWriteDmu(2, kDpgColourGY + o, 0);
			regWriteDmu(2, kDpgColourBCb + o, 0);
			regWriteDmu(2, kDpgDimensions + o,
			            ((fbWidth & 0x3fff) << 16) | (fbHeight & 0x3fff));
			regWriteDmu(2, kDpgControl + o,
			            (dpgSavedControl & ~0x71u) | (kDpgModeHorizontalBars << 4) | 1u);
		} else {
			regWriteDmu(2, kDpgControl + o, dpgSavedControl & ~1u);
		}
		FBLOG("power: HDMI display %s via DPG on OPP%u (ctl 0x%08x)",
		      on ? "unblanked" : "blanked", pipe.opp, regReadDmu(2, kDpgControl + o));
		{
			char res[40];
			snprintf(res, sizeof(res), "DPG ctl 0x%08x", regReadDmu(2, kDpgControl + o));
			displayPowerNote(on, "HDMI DPG", res);
		}
		displayPowerOn = on;
		return;
	}

	// DP stream encoder of the boot pipe (DP0 unless discovery found
	// another DIG; dcn_4_1_0_offset.h, base_idx 2, DIG stride).
	const uint32_t kDpVidStreamCntl = 0x2122 + digOff();
	constexpr uint32_t kVidStreamEnable = 1u << 0;
	// DPCD SET_POWER (native AUX address 0x600): D0 = 1, D3/sleep = 2.
	constexpr uint32_t kDpcdSetPower = 0x600;

	auto sinkPower = [&](uint8_t state) -> bool {
		if (!sinkAuxValid)
			return false;
		// A sink coming out of D3 may need a moment before it ACKs AUX
		// (DP spec allows up to 1 ms; be generous).
		for (int t = 0; t < 10; t++) {
			uint8_t rb = 0;
			int rc = auxTransaction(sinkAuxInst, kActDpWrite, kDpcdSetPower,
			                        &state, 1, nullptr, 0, &rb);
			if (rc == kReplyAck)
				return true;
			IOSleep(1);
		}
		return false;
	};

	uint32_t v = regReadDmu(2, kDpVidStreamCntl);
	if (v == 0xFFFFFFFF) {
		FBLOG("power: stream register unreadable, leaving display alone");
		displayPowerNote(on, "DP", "stream register unreadable, display left alone");
		return;
	}

	if (on) {
		// Wake the sink first so it sees video the moment the stream returns.
		bool acked = sinkPower(0x1);
		regWriteDmu(2, kDpVidStreamCntl, v | kVidStreamEnable);
		FBLOG("power: display on (sink D0 %s, stream 0x%08x -> 0x%08x)",
		      acked ? "acked" : "no ack", v, regReadDmu(2, kDpVidStreamCntl));
		{
			char res[56];
			snprintf(res, sizeof(res), "sink D0 %s, stream 0x%08x", acked ? "acked" : "no ack", regReadDmu(2, kDpVidStreamCntl));
			displayPowerNote(true, "DP stream", res);
		}
	} else {
		// Blank the stream, then let the sink drop to D3. The timing
		// generator keeps running; only the video stream enable is touched.
		regWriteDmu(2, kDpVidStreamCntl, v & ~kVidStreamEnable);
		bool acked = sinkPower(0x2);
		FBLOG("power: display off (stream 0x%08x -> 0x%08x, sink D3 %s)",
		      v, regReadDmu(2, kDpVidStreamCntl), acked ? "acked" : "no ack");
		{
			char res[56];
			snprintf(res, sizeof(res), "sink D3 %s, stream 0x%08x", acked ? "acked" : "no ack", regReadDmu(2, kDpVidStreamCntl));
			displayPowerNote(false, "DP stream", res);
		}
	}
	displayPowerOn = on;
}


namespace {
// OTG global-sync / update-lock registers of instance 0 (dcn_4_1_0_offset.h,
// base_idx 2, OTG stride 0x80). Pipe updates are double-buffered: writes go
// to a pending copy that latches into live hardware on the VUPDATE pulse,
// and only while OTG_MASTER_UPDATE_LOCK is released. The GOP holds the lock
// and programs no pulse, so without ensureUpdateLatch() nothing written to
// the pipe after boot ever takes effect (writes read back fine regardless).
constexpr uint32_t kOtgMasterUpdateLock  = 0x1b89;   // LOCK [0], UPDATE_LOCK_STATUS [8]
constexpr uint32_t kOtgDoubleBufferCtl   = 0x1b5c;
constexpr uint32_t kDppTopControl        = 0x0cc5;   // DPP_TOP0_DPP_CONTROL (log only)
constexpr uint32_t kOtgVStartupParam     = 0x1b85;
constexpr uint32_t kOtgVUpdateParam      = 0x1b86;   // VUPDATE_WIDTH [25:16]
constexpr uint32_t kOtgVReadyParam       = 0x1b87;
constexpr uint32_t kOtgGlobalSyncStatus  = 0x1b88;   // VUPDATE_EVENT_OCCURRED [8]
} // namespace

void RDNA4Device::ensureUpdateLatch() {
	if (updateLatchReady || !ipDiscovery.isValid() || !rmmio)
		return;
	const uint32_t o = otgOff();

	// Release the OTG master update lock if the GOP left it held: with the
	// lock asserted, every double-buffered pipe write stays pending forever
	// (writes read back fine, hardware never changes).
	uint32_t lock = regReadDmu(2, kOtgMasterUpdateLock + o);
	latchNote("OTG%u lock=0x%08x (status=%u) dbufctl=0x%08x dppctl=0x%08x",
	      pipe.otg < Pipe::kMaxOtg ? pipe.otg : 0, lock, (lock >> 8) & 1,
	      regReadDmu(2, kOtgDoubleBufferCtl + o), regReadDmu(2, kDppTopControl + dppOff()));
	if (lock == 0xFFFFFFFF)
		return;
	if (lock & 1) {
		regWriteDmu(2, kOtgMasterUpdateLock + o, 0);
		latchNote("released OTG master update lock (was 0x%08x, now 0x%08x)",
		      lock, regReadDmu(2, kOtgMasterUpdateLock + o));
	}

	// Ensure the VUPDATE latch pulse exists. Without it, no pipe update we
	// ever queue becomes live.
	uint32_t vstartup = regReadDmu(2, kOtgVStartupParam + o);
	uint32_t vupdate  = regReadDmu(2, kOtgVUpdateParam + o);
	uint32_t sync     = regReadDmu(2, kOtgGlobalSyncStatus + o);
	latchNote("global sync: vstartup=0x%08x vupdate=0x%08x vready=0x%08x "
	      "status=0x%08x (vupdate_occurred=%u)",
	      vstartup, vupdate, regReadDmu(2, kOtgVReadyParam + o), sync,
	      (sync >> 8) & 1);
	if (((vupdate >> 16) & 0x3ff) == 0) {
		// Pulse of 2 lines right at VSTARTUP. If VSTARTUP is also
		// unprogrammed, place it inside the vertical blank of the live mode
		// (upstream used 40 lines, sized for its 4K display's 62-line blank).
		if ((vstartup & 0x3ff) == 0) {
			uint32_t start = 40;
			if (bootTimingValid && bootTiming.vBlank > 4 && bootTiming.vBlank - 2 < start)
				start = bootTiming.vBlank - 2u;
			regWriteDmu(2, kOtgVStartupParam + o, start);
		}
		regWriteDmu(2, kOtgVUpdateParam + o, (2u << 16));
		latchNote("programmed VUPDATE pulse (vstartup=0x%08x vupdate=0x%08x)",
		      regReadDmu(2, kOtgVStartupParam + o), regReadDmu(2, kOtgVUpdateParam + o));
	}
	updateLatchReady = true;
}


bool RDNA4Device::regWriteDmu(uint8_t baseIdx, uint32_t dwordOffset, uint32_t value) {
	uint32_t byteOffset;
	if (!ipDiscovery.isValid() ||
	    !ipDiscovery.regByteOffset(IpDiscovery::HwDmu, 0, baseIdx, dwordOffset, byteOffset))
		return false;
	if (!rmmio || byteOffset + 4 > rmmioSize)
		return false;
	rmmio[byteOffset / 4] = value;
	return true;
}

void RDNA4Device::tryForce8bpc() {
	uint32_t v = 0;
	if (!PE_parse_boot_argn("rdna4-8bpc", &v, sizeof(v)) || v == 0)
		return;

	// The firmware does not always light the DP display on DIG0.
	if (!pipe.valid() || pipe.signal != Pipe::Signal::DpSst) {
		FBLOG("8bpc: the lit pipe is not DisplayPort: nothing done");
		return;
	}
	// The lit pipe's DP stream encoder (DP0's registers, base 2, DIG stride).
	const uint32_t kDpPixelFormat    = 0x211f + digOff();
	const uint32_t kDpMsaColorimetry = 0x2120 + digOff();
	constexpr uint32_t kDepthMask       = 0x00000700; // UNCOMPRESSED_COMPONENT_DEPTH
	constexpr uint32_t kDepth8bpc       = 1u << 8;
	constexpr uint32_t kMisc0Mask       = 0xFF000000; // MISC0 in [31:24]
	constexpr uint32_t kMisc0Rgb8bpc    = 0x20u << 24; // bits[7:5]=001 -> 8 bpc, RGB

	uint32_t pf  = regReadDmu(2, kDpPixelFormat);
	uint32_t col = regReadDmu(2, kDpMsaColorimetry);
	if (pf == 0xFFFFFFFF || col == 0xFFFFFFFF) {
		FBLOG("8bpc: register read failed, aborting");
		return;
	}

	uint32_t pfNew  = (pf  & ~kDepthMask) | kDepth8bpc;
	uint32_t colNew = (col & ~kMisc0Mask) | kMisc0Rgb8bpc;
	FBLOG("8bpc: DP%u: DP_PIXEL_FORMAT 0x%08x -> 0x%08x, MSA_COLORIMETRY 0x%08x -> 0x%08x",
	      pipe.dig, pf, pfNew, col, colNew);
	regWriteDmu(2, kDpPixelFormat, pfNew);
	regWriteDmu(2, kDpMsaColorimetry, colNew);
	FBLOG("8bpc: readback DP_PIXEL_FORMAT=0x%08x MSA_COLORIMETRY=0x%08x",
	      regReadDmu(2, kDpPixelFormat), regReadDmu(2, kDpMsaColorimetry));
}

namespace {
RDNA4Device *gSleepTestDev;   // the device debug.rdna4_head2sleep acts on

int head2SleepSysctl(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	int value = 0;
	const int err = sysctl_handle_int(oidp, &value, 0, req);
	if (err || !req->newptr)
		return err;
	if ((value == 1 || value == 2) && gSleepTestDev)
		gSleepTestDev->head2SleepTest(value);
	return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, rdna4_head2sleep, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, head2SleepSysctl, "I",
	    "AMDRadeonRDNA4: 1 puts the second display's pipe to sleep, 2 wakes it, each register step a breadcrumb");

RDNA4Device *gColdDev;   // the device debug.rdna4_coldreset acts on (rdna4-coldprobe=1)

int coldResetSysctl(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	int value = 0;
	const int err = sysctl_handle_int(oidp, &value, 0, req);
	if (err || !req->newptr)
		return err;
	if (value == 1 && gColdDev)
		(void)gColdDev->coldReset();
	return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, rdna4_coldreset, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, coldResetSysctl, "I",
	    "AMDRadeonRDNA4: 1 resets the card (mode 1) and brings it back as Linux does before its IP blocks start");
} // namespace

// A real cold start, stage (a): the state a mode-1 reset (amdgpu_device_mode1_reset: SMU debug mailbox
// DEBUGSMC_MSG_Mode1Reset on MP1 14.0.2/3) and the VBIOS's asic_init (its bytecode, decoded 2026-11-15) depend on,
// logged and nothing written. The PSP's bootloader and mailbox words, the SMU's debug mailbox, the registers
// asic_init reads and writes, the VRAM size, and the PCI config space a reset clears (the BARs, the command
// register, PCIe link control, Resizable BAR), to compare with after a reset.
void RDNA4Device::coldProbe() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("rdna4-coldprobe", &on, sizeof(on)) || !on || !ipDiscovery.isValid() || !rmmio)
		return;
	if (!gColdDev) {
		gColdDev = this;
		sysctl_register_oid(&sysctl__debug_rdna4_coldreset);
	}
	auto ip = [&](IpDiscovery::HwId hw, uint32_t seg, uint32_t dword) -> uint32_t {
		uint32_t off;
		if (!ipDiscovery.regByteOffset(hw, 0, seg, dword, off) || off + 4 > rmmioSize)
			return 0xFFFFFFFF;
		return rmmio[off / 4];
	};
	auto mm = [&](uint32_t dword) { return dword * 4 + 4 <= rmmioSize ? rmmio[dword] : 0xFFFFFFFFu; };
	// MP0 C2PMSG_n at dword 0x40 + n (segment 0): 33 VMBX, 35 bootloader, 64 ring, 81 sOS
	FBLOG("cold: psp C2PMSG_33 0x%08x (asic_init waits for bit 31), _35 0x%08x (bootloader, bit 31), _64 0x%08x, "
	      "_81 0x%08x (sOS)", ip(IpDiscovery::HwMp0, 0, 0x61), ip(IpDiscovery::HwMp0, 0, 0x63),
	      ip(IpDiscovery::HwMp0, 0, 0x80), ip(IpDiscovery::HwMp0, 0, 0x91));
	// MP1 C2PMSG_n at dword 0x40 + n (segment 1): the debug mailbox's param 53, response 54, message 75
	FBLOG("cold: smu debug mailbox: param(C2PMSG_53) 0x%08x resp(_54) 0x%08x msg(_75) 0x%08x",
	      ip(IpDiscovery::HwMp1, 1, 0x75), ip(IpDiscovery::HwMp1, 1, 0x76), ip(IpDiscovery::HwMp1, 1, 0x8b));
	FBLOG("cold: asic_init's registers: [0x0006] 0x%08x [0x004c] 0x%08x [0x004e] 0x%08x [0x0050] 0x%08x "
	      "[0x0053] 0x%08x [0x0de3] 0x%08x", mm(0x0006), mm(0x004c), mm(0x004e), mm(0x0050), mm(0x0053), mm(0x0de3));
	logPciState("probe");
}

void RDNA4Device::logPciState(const char *when) {
	if (rmmio && ipDiscovery.isValid()) {
		uint32_t off = 0;
		auto ip = [&](uint16_t hw, uint8_t seg, uint32_t dword) {
			return ipDiscovery.regByteOffset(hw, 0, seg, dword, off) && off + 4 <= rmmioSize ? rmmio[off / 4] : 0xdeadbeefu;
		};
		FBLOG("cold: %s: mmio: VRAM size 0x%08x, C2PMSG_35 0x%08x (bootloader), C2PMSG_81 0x%08x (sOS)", when,
		      ip(IpDiscovery::HwNbif, 2, 0x00c3), ip(IpDiscovery::HwMp0, 0, 0x63), ip(IpDiscovery::HwMp0, 0, 0x91));
	}
	if (!pciDevice)
		return;
	uint32_t cfg[16];
	for (uint32_t i = 0; i < 16; i++)
		cfg[i] = pciDevice->configRead32(static_cast<uint8_t>(i * 4));
	FBLOG("cold: %s: pci 00-1c: %08x %08x %08x %08x %08x %08x %08x %08x", when, cfg[0], cfg[1], cfg[2], cfg[3], cfg[4], cfg[5],
	      cfg[6], cfg[7]);
	FBLOG("cold: %s: pci 20-3c: %08x %08x %08x %08x %08x %08x %08x %08x", when, cfg[8], cfg[9], cfg[10], cfg[11], cfg[12],
	      cfg[13], cfg[14], cfg[15]);
	IOByteCount pcie = 0, rebar = 0;
	if (pciDevice->extendedFindPCICapability(0x10, &pcie) && pcie)   // PCI Express: device and link control/status
		FBLOG("cold: %s: pcie cap@0x%x: devctl/sta 0x%08x linkcap 0x%08x linkctl/sta 0x%08x devctl2 0x%08x linkctl2 "
		      "0x%08x", when, static_cast<uint32_t>(pcie), pciDevice->configRead32(static_cast<uint8_t>(pcie + 0x08)),
		      pciDevice->configRead32(static_cast<uint8_t>(pcie + 0x0c)),
		      pciDevice->configRead32(static_cast<uint8_t>(pcie + 0x10)),
		      pciDevice->configRead32(static_cast<uint8_t>(pcie + 0x28)),
		      pciDevice->configRead32(static_cast<uint8_t>(pcie + 0x30)));
	// Resizable BAR (extended capability 0x15): a capability and a control word for each of up to six BARs
	if (pciDevice->extendedFindPCICapability(-0x15, &rebar) && rebar)
		for (uint32_t i = 0; i < 6; i++) {
			const uint32_t cap = pciDevice->extendedConfigRead32(rebar + 4 + 8 * i),
			               ctl = pciDevice->extendedConfigRead32(rebar + 8 + 8 * i);
			if (i && !(cap | ctl))
				break;
			FBLOG("cold: %s: rebar cap@0x%x entry %u: cap 0x%08x ctl 0x%08x (BAR %u, size 2^%u MiB)",
			      when, static_cast<uint32_t>(rebar), i, cap, ctl, ctl & 7, (ctl >> 8) & 0x3f);
		}
	// PCI power management (capability 0x01): PMCSR bits 1:0 are the D state.
	IOByteCount pm = 0;
	if (pciDevice->extendedFindPCICapability(0x01, &pm) && pm) {
		const uint32_t pmcsr = pciDevice->configRead32(static_cast<uint8_t>(pm + 4));
		FBLOG("cold: %s: pm cap@0x%x: PMCSR 0x%08x (D%u)", when, static_cast<uint32_t>(pm), pmcsr, pmcsr & 3);
	}
	// The card's own PCIe switch: its downstream port above the GPU, and the upstream port above that.
	IOService *up = pciDevice;
	for (int level = 1; level <= 2; level++) {
		IOService *bridge = up->getProvider();
		auto *dev = bridge ? OSDynamicCast(IOPCIDevice, bridge->getProvider()) : nullptr;
		if (!dev)
			break;
		FBLOG("cold: %s: bridge %d (%s): id %08x command/status %08x buses %08x memory %08x prefetch %08x %08x %08x",
		      when, level, dev->getName(), dev->configRead32(0x00), dev->configRead32(0x04), dev->configRead32(0x18),
		      dev->configRead32(0x20), dev->configRead32(0x24), dev->configRead32(0x28), dev->configRead32(0x2c));
		up = dev;
	}
}

// The runtime's sleep and wake come from this device's notifiers (no rdna4-pm, which puts it in the power tree).
bool RDNA4Device::drivesRuntimePower() const { return compute && !RDNA4Compute::requestedPowerManagement(); }

void RDNA4Device::captureGopState() {
	if (!bootDpLinkKnown)
		noteBootDpLink();
	coldGopSurface = bootScanout();
	coldGopTimer = regReadDmu(2, DcnInit::kTimerCntl);
	FBLOG("cold: the GOP's state kept for a cold start: scanout 0x%llx, DCHUB timer 0x%08x, boot link %s", coldGopSurface,
	      coldGopTimer, bootDpLinkKnown ? "known" : "NOT known");
}

// Roadmap item 5, stage 2: a word at the console's start written before sleep and read once woken (whether VRAM
// kept its contents through the power loss), and from the wake on the card treated as after a cold reset: the
// compute bring-up runs again (rdna4-pm=1) and its SMU step runs the cold start (coldStartHeads) in cold mode.
IOReturn RDNA4Device::sleepWakeMessage(void *target, void *, UInt32 type, IOService *, void *, vm_size_t) {
	auto *dev = static_cast<RDNA4Device *>(target);
	auto vramWord = [dev](bool write, uint32_t value) -> uint32_t {
		IODeviceMemory *bar0 = dev->pciDevice ? dev->pciDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0) : nullptr;
		IOMemoryMap *map = bar0 ? bar0->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, 0, 4096) : nullptr;
		volatile uint32_t *w = map ? reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()) : nullptr;
		uint32_t got = 0xdeadbeef;
		if (w && write)
			w[0] = value;
		if (w)
			got = w[0];
		if (map)
			map->release();
		return got;
	};
	if (type == kIOMessageSystemWillSleep) {
		dev->logPciState("before sleep");
		dev->vramMark = vramWord(true, 0x5a5a1234) == 0x5a5a1234 ? 0x5a5a1234 : 0;
		if (dev->drivesRuntimePower()) {   // the engines quiesced while the card has power
			dev->compute->powerSleepRequest();
			dev->compute->powerWillSleep();
		}
	} else if (type == kIOMessageSystemWillPowerOn) {
		dev->logPciState("waking");
		dev->coldBoot = dev->coldStartPending = true;
		FBLOG("cold: waking: the card lost its power: the next cold start runs as after a reset");
	} else if (type == kIOMessageSystemWillNotSleep) {   // a sleep called off after the engines were quiesced
		FBLOG("cold: the sleep was called off: the runtime comes back");
		if (dev->drivesRuntimePower())
			dev->compute->powerDidWake();
	} else if (type == kIOMessageSystemHasPoweredOn) {
		dev->logPciState("woken");
		if (dev->drivesRuntimePower())   // if the card's power-on was not seen (it did nothing then)
			dev->compute->powerDidWake();
		if (dev->vramMark) {
			const uint32_t got = vramWord(false, 0);
			FBLOG("cold: woken: VRAM %s through the sleep (0x%08x)", got == dev->vramMark ? "kept its contents" : "lost its contents", got);
		}
	}
	return kIOReturnSuccess;
}


// Stage (b): amdgpu_device_mode1_reset as Linux runs it on MP1 14.0.2/3, then the VBIOS's asic_init. Not the
// IP blocks: the PSP's sOS, the SMU's features, DMUB, the display and the compute runtime are as dead afterwards as
// after a power loss, and the displays dark: this shows whether the card comes back to where a cold start begins.
bool RDNA4Device::coldReset() {
	if (!pciDevice || !rmmio || !ipDiscovery.isValid())
		return false;
	auto ipOff = [&](uint16_t hw, uint8_t seg, uint32_t dword) {
		uint32_t off = 0;
		return ipDiscovery.regByteOffset(hw, 0, seg, dword, off) && off + 4 <= rmmioSize ? off / 4 : 0u;
	};
	const uint32_t c2p33 = ipOff(IpDiscovery::HwMp0, 0, 0x61), c2p35 = ipOff(IpDiscovery::HwMp0, 0, 0x63),
	               c2p81 = ipOff(IpDiscovery::HwMp0, 0, 0x91);
	const uint32_t dbgParam = ipOff(IpDiscovery::HwMp1, 1, 0x75), dbgResp = ipOff(IpDiscovery::HwMp1, 1, 0x76),
	               dbgMsg = ipOff(IpDiscovery::HwMp1, 1, 0x8b);
	uint32_t memsizeOff = 0;
	if (!c2p33 || !c2p35 || !dbgParam || !dbgMsg || !dbgResp ||
	    !ipDiscovery.regByteOffset(IpDiscovery::HwNbif, 0, 2, 0x00c3, memsizeOff)) {
		FBLOG("cold: reset not done: a register is not in the discovery table");
		return false;
	}
	// VRAM before: a pattern at the console's first page, to see afterwards whether memory kept it.
	IODeviceMemory *bar0 = pciDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	IOMemoryMap *vmap = bar0 ? bar0->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, 0, 4096)
	                         : nullptr;
	volatile uint32_t *vram = vmap ? reinterpret_cast<volatile uint32_t *>(vmap->getVirtualAddress()) : nullptr;
	if (vram)
		vram[0] = 0x5a5a1234;

	// amdgpu_device_cache_pci_state: the header and capabilities, and Resizable BAR (extended capability 0x15).
	uint32_t cfg[64];
	for (uint32_t i = 0; i < 64; i++)
		cfg[i] = pciDevice->configRead32(static_cast<uint8_t>(i * 4));
	IOByteCount rebar = 0;
	uint32_t rebarCtl[6] = {};
	uint32_t nrebar = 0;
	if (pciDevice->extendedFindPCICapability(-0x15, &rebar) && rebar)
		for (; nrebar < 6; nrebar++) {
			rebarCtl[nrebar] = pciDevice->extendedConfigRead32(rebar + 8 + 8 * nrebar);
			if (nrebar && !rebarCtl[nrebar])
				break;
		}
	FBLOG("cold: resetting the card (SMU debug mailbox, DEBUGSMC_MSG_Mode1Reset): command 0x%04x, BAR0 0x%08x%08x, "
	      "%u Resizable BAR control(s)", cfg[1] & 0xffff, cfg[5], cfg[4], nrebar);

	// pci_clear_master, then smu_v14_0_2_mode1_reset: param, message, response cleared; 1 s without the registers.
	pciDevice->configWrite16(kIOPCIConfigCommand, static_cast<uint16_t>((cfg[1] & 0xffff) & ~0x4u));
	rmmio[dbgParam] = 0;
	rmmio[dbgMsg] = 2;   // DEBUGSMC_MSG_Mode1Reset
	rmmio[dbgResp] = 0;
	IOSleep(1000);

	// amdgpu_device_load_pci_state: the BAR sizes first, then the header (BARs before the command register).
	for (uint32_t i = 0; i < nrebar; i++)
		pciDevice->extendedConfigWrite32(rebar + 8 + 8 * i, rebarCtl[i]);
	for (uint32_t i = 2; i < 64; i++)   // past the IDs and the command/status word
		if (i != 1)
			pciDevice->configWrite32(static_cast<uint8_t>(i * 4), cfg[i]);
	pciDevice->configWrite16(kIOPCIConfigCommand, static_cast<uint16_t>(cfg[1] & 0xffff));

	// amdgpu_psp_wait_for_bootloader (C2PMSG_35 bit 31), then the VRAM size readable again.
	auto waitFor = [&](uint32_t dword, uint32_t mask, uint32_t want, uint32_t ms) {
		for (uint32_t t = 0; t < ms; t++) {
			if ((rmmio[dword] & mask) == want)
				return true;
			IOSleep(1);
		}
		return false;
	};
	const bool boot = waitFor(c2p35, 0x80000000, 0x80000000, 5000);
	bool sized = false;
	for (uint32_t t = 0; t < 5000 && !sized; t++) {
		const uint32_t v = rmmio[memsizeOff / 4];
		sized = v && v != 0xffffffff;
		if (!sized)
			IOSleep(1);
	}
	FBLOG("cold: after the reset: bootloader %s (C2PMSG_35 0x%08x), VRAM size %u MiB, C2PMSG_33 0x%08x, sOS 0x%08x, "
	      "command 0x%04x, BAR0 0x%08x%08x", boot ? "ready" : "NOT ready", rmmio[c2p35], rmmio[memsizeOff / 4],
	      rmmio[c2p33], rmmio[c2p81], pciDevice->configRead16(kIOPCIConfigCommand),
	      pciDevice->configRead32(kIOPCIConfigBaseAddress1), pciDevice->configRead32(kIOPCIConfigBaseAddress0));

	// The VBIOS's asic_init (table 0, decoded): wait for C2PMSG_33 to read 0x80000000 (4 s); if it does not and the
	// strap (REG 0x50 bits 31:24) is 1, ask the bootloader with 0x70000 in C2PMSG_35 and wait again; then the tail:
	// REG 6 = (REG 0xde3 - 1) >> 11, REG 0x53 |= 0x200.
	const uint32_t strap = rmmio[0x50] & 0xff000000;
	bool vmbx = waitFor(c2p33, 0xffffffff, 0x80000000, 4000);
	if (!vmbx && strap == 0x01000000) {
		rmmio[c2p35] = 0x70000;
		const bool done = waitFor(c2p35, 0xffffffff, 0x80000000, 4000);
		rmmio[0x50] &= 0x00ffffff;
		vmbx = done && waitFor(c2p33, 0xffffffff, 0x80000000, 4000);
	}
	const uint32_t de3 = rmmio[0x0de3];
	if (de3) {
		rmmio[0x0006] = (de3 - 1) >> 11;
		rmmio[0x0053] |= 0x200;
	}
	FBLOG("cold: asic_init: C2PMSG_33 %s (0x%08x, strap 0x%08x); [0x0006] 0x%08x [0x0053] 0x%08x [0x0de3] 0x%08x",
	      vmbx ? "ready" : "NOT ready", rmmio[c2p33], strap, rmmio[0x0006], rmmio[0x0053], de3);

	// VRAM after: whether the pattern survived, and whether a new one reads back.
	if (vram) {
		const uint32_t kept = vram[0];
		vram[1] = 0xa5a5c3c3;
		FBLOG("cold: VRAM: the pattern written before %s (0x%08x); a new one reads back %s (0x%08x)",
		      kept == 0x5a5a1234 ? "kept" : "gone", kept, vram[1] == 0xa5a5c3c3 ? "right" : "WRONG", vram[1]);
	}
	if (vmap)
		vmap->release();
	return boot && sized && vmbx;
}

// The card's IOPCIDevice powered on again after a sleep: IOPCIFamily has restored its config space, so its registers
// answer (unposted). The runtime's wake bring-up (on its own thread) runs from here without rdna4-pm.
IOReturn RDNA4Device::pciPowerMessage(void *target, void *, UInt32 type, IOService *, void *, vm_size_t) {
	auto *dev = static_cast<RDNA4Device *>(target);
	if (type == kIOMessageDeviceHasPoweredOn) {
		dev->logPciState("pci powered on");
		// The card powered off with the system (it has no power states of its own): it lost its power. Said here too, not only
		// at kIOMessageSystemWillPowerOn: in a dark wake (the system woken by a PCIe PME, a USB key press here) the card powers
		// on long before that message, which only comes with the full wake, and the bring-up below would run warm (no displays).
		if (!dev->coldBoot)
			FBLOG("cold: pci powered on: the card lost its power: the next cold start runs as after a reset");
		dev->coldBoot = dev->coldStartPending = true;
		if (dev->drivesRuntimePower())
			dev->compute->powerDidWake();
	}
	return kIOReturnSuccess;
}

void RDNA4Device::probeMemSize() {
	// RCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE (NBIF 6.3.1 seg 2, dword 0x00c3):
	// VRAM size in MiB — amdgpu's nbif_v6_3_1_get_memsize. The byte offset
	// below was derived from this card's IP discovery table (base 0xd20)
	// and is used as the fallback when discovery isn't available at runtime.
	uint32_t off = 0x378c;
	uint32_t discOff;
	if (ipDiscovery.isValid() &&
	    ipDiscovery.regByteOffset(IpDiscovery::HwNbif, 0, 2, 0x00c3, discOff)) {
		if (discOff != off)
			FBLOG("mmio: discovery moved RCC_CONFIG_MEMSIZE to 0x%x", discOff);
		off = discOff;
	}

	uint32_t memsizeMB = regRead32(off);
	if (memsizeMB == 0 || memsizeMB == 0xFFFFFFFF) {
		FBLOG("mmio: RCC_CONFIG_MEMSIZE read failed (0x%08x) — MMIO not usable", memsizeMB);
		return;
	}

	FBLOG("mmio: VRAM size %u MiB (RCC_CONFIG_MEMSIZE @ 0x%x)", memsizeMB, off);
	owner->setProperty("VRAM,TotalMB", static_cast<uint64_t>(memsizeMB), 32);
	// System Information's "VRAM (Total)" comes from these properties on the
	// GPU's PCI device; without them macOS shows the framebuffer size (the
	// "7 MB" of an unsupported card). The register counts usable VRAM, i.e.
	// the nominal size minus firmware reservations: report the whole GiB.
	uint64_t nominalMB = (static_cast<uint64_t>(memsizeMB) + 1023) / 1024 * 1024;
	uint64_t nominalBytes = nominalMB << 20;
	pciDevice->setProperty("VRAM,totalMB", nominalMB, 32);
	pciDevice->setProperty("VRAM,totalsize", &nominalBytes, sizeof(nominalBytes));
	// Independent confirmation that register MMIO works — the gate for all
	// future DCN (AUX/EDID, mode setting) work. The register reports usable
	// VRAM (nominal size minus firmware reservations: 16304 on this 16 GiB
	// card), so accept any plausible value rather than an exact match.
	owner->setProperty("MMIO,Verified", memsizeMB >= 1024 && memsizeMB <= 65536);
}


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool RDNA4Device::init(IOPCIDevice *pci, IOService *ownerService) {
	pciDevice = pci;
	owner = ownerService;
	if (!pciDevice || !owner)
		return false;
	if (!extraLock)
		extraLock = IORecursiveLockAlloc();

	uint32_t nosleep = 0;
	if (PE_parse_boot_argn("rdna4-nosleep", &nosleep, sizeof(nosleep)) && nosleep != 0) {
		displaySleepEnabled = false;
		FBLOG("init: display sleep disabled by rdna4-nosleep");
	}

	// The scanout the GOP left behind is what IONDRVFramebuffer shows too.
	if (!captureConsoleInfo()) {
		FBLOG("init: could not capture console framebuffer, leaving the display alone");
		return false;
	}

	uint32_t vd = pciDevice->configRead32(kIOPCIConfigVendorID);
	isAmd = (vd & 0xffff) == 0x1002;
	uint32_t cursor = 0;
	ndrvCursorRequested = PE_parse_boot_argn("rdna4-cursor", &cursor, sizeof(cursor)) && cursor != 0;
	// The plane for the paravirtual displays' cursor (PvScanout::cursorReady), without rdna4-cursor's NDRV routes: on where the paravirtual
	// GPU is built in (card-verified 2026-11-19: one arrow per display, Safari's box around the pointer gone), rdna4-pvcursor=0 turns it off.
#ifdef AMDRadeonRDNA4_PVGPU
	uint32_t pvCursor = 1;
#else
	uint32_t pvCursor = 0;
#endif
	(void)Trial::option("rdna4-pvcursor", pvCursor);
	hwCursorRequested = ndrvCursorRequested || pvCursor != 0;
	if (hwCursorRequested)
		FBLOG("cursor: hardware cursor plane requested (%s)", ndrvCursorRequested ? "rdna4-cursor=1" : "the paravirtual displays'");

	const uint16_t device = static_cast<uint16_t>(vd >> 16);
	const uint8_t revision = pciDevice->configRead8(kIOPCIConfigRevisionID);
	const char *model = Sku::modelName(device, revision);

	if (!isAmd)
		model = "VM test (vmware-svga)";
	else
		pciDevice->setProperty("model", model);   // System Report / ioreg
	owner->setProperty("GPU,Variant", model);

	if (isAmd) {
		// Enable memory space so the aperture is reachable; never bus
		// mastering — nothing here issues DMA.
		pciDevice->setMemoryEnable(true);

		// Registers first: the on-die discovery fallback needs MMIO.
		bool haveMmio = mapRegisters();

		// Connector layout and firmware info for mode setting; the display
		// itself does not depend on this.
		loadVBIOS();
		if (haveMmio && !ipDiscovery.isValid())
			loadOnDieDiscovery();

		if (haveMmio) {
			// First: every per-pipe path below keys off the pipe the GOP lit.
			discoverPipe();
			if (hwCursorRequested)
				initHardwareCursor();
			probeMemSize();
			coldProbe();
			dumpDCN();
			tryForce8bpc();   // DP-stream experiment (rdna4-8bpc=1)
			probeEDID();
			dumpModeState();
			dmubHistory();
			dumpDmubVersion();
			dmubPing();
			smuPing();
			dumpIH();
			dumpPSP();
		}
	}

	// The extra pipes drive DisplayPort sinks (trained on the card, two
	// Dells, 2026-11-04); rdna4-head2dp=0 leaves those connectors to HDMI.
	uint32_t head2dp = 1;

	(void)Trial::option("rdna4-head2dp", head2dp);
	pipe2DpAllowed = head2dp != 0;

	// Needs the lit pipe (discoverPipe) and the sink's AUX channel (probeEDID).
	if (PE_parse_boot_argn("rdna4-dptrain", &dpTrainLevel, sizeof(dpTrainLevel)) && dpTrainLevel)
		noteBootDpLink();

	// Needs the boot timing (discoverPipe) and the sink's EDID (probeEDID).
	buildModeTable();

	// rdna4-coldboot=1: what the GOP left read while it is there, then the card reset (coldReset).
	uint32_t coldArg = 0;
	if (PE_parse_boot_argn("rdna4-coldboot", &coldArg, sizeof(coldArg)) && coldArg && rmmio && pipe.valid()) {
		captureGopState();
		coldBoot = coldReset();
		FBLOG("cold: boot: %s (the GOP's scanout 0x%llx, timer 0x%08x)",
		      coldBoot ? "the card reset; the displays light once the SMU runs" : "the reset FAILED: as a warm boot",
		      coldGopSurface, coldGopTimer);
	}
	if (!gSleepTestDev && rmmio) {
		gSleepTestDev = this;
		sysctl_register_oid(&sysctl__debug_rdna4_head2sleep);
	}
	if (systemSleepWanted() && !sleepWakeNotifier) {
		if (rmmio && pipe.valid() && !coldBoot)   // what a cold start after the wake compares against
			captureGopState();
		if (IOPMrootDomain *root = IOService::getPMRootDomain())
			sleepWakeNotifier = root->registerInterest(gIOPriorityPowerStateInterest, sleepWakeMessage, this, nullptr);
		if (pciDevice)
			pciPowerNotifier = pciDevice->registerInterest(gIOGeneralInterest, pciPowerMessage, this, nullptr);
		FBLOG("cold: the card's PCI state is logged at sleep and wake: %s, its power-on %s",
		      sleepWakeNotifier ? "registered" : "NOT registered", pciPowerNotifier ? "watched" : "NOT watched");
		logPciState("boot");
	}
	FBLOG("init: device ready (%ux%u console, %lu mode(s))", fbWidth, fbHeight,
	      static_cast<unsigned long>(modeCount));
	return true;
}

static uint32_t gSnapLines;   // wakediff lines of the current comparison

RDNA4Device::~RDNA4Device() {
	freeHardwareCursor();
	if (cursorTrailLock) {
		IOLockFree(cursorTrailLock);
		cursorTrailLock = nullptr;
	}
	if (extraLock) {
		IORecursiveLockFree(extraLock);
		extraLock = nullptr;
	}
	if (onDieDisc) {
		IOFree(onDieDisc, 10 << 10);
		onDieDisc = nullptr;
	}
	unmapRegisters();
	freeVBIOS();
}

// ---------------------------------------------------------------------------
// Mode table
// ---------------------------------------------------------------------------

#ifdef AMDRadeonRDNA4_VM_TEST
// Lenovo G25-10 base block (the test/atomdump.cpp fixture). With boot-arg
// "rdna4-fakeedid=1" the VM test build serves it as the sink's EDID, so the
// mode table and how a given macOS release presents it can be checked
// without RDNA 4 hardware.
static const uint8_t kVmFixtureEdid[128] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x30, 0xae, 0xfe, 0x65, 0x00, 0x00, 0x00, 0x00,
	0x32, 0x1e, 0x01, 0x03, 0x80, 0x36, 0x1e, 0x78, 0x2a, 0x90, 0x55, 0xa7, 0x55, 0x53, 0xa0, 0x28,
	0x13, 0x50, 0x54, 0xa1, 0x08, 0x00, 0xd1, 0xc0, 0xb3, 0x00, 0x81, 0xc0, 0x81, 0x80, 0x95, 0x00,
	0xa9, 0xc0, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
	0x45, 0x00, 0x20, 0x2f, 0x21, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x30, 0x90, 0x1e,
	0xaa, 0x22, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x4c,
	0x45, 0x4e, 0x20, 0x47, 0x32, 0x35, 0x2d, 0x31, 0x30, 0x0a, 0x20, 0x20, 0x00, 0x00, 0x00, 0xff,
	0x00, 0x55, 0x34, 0x42, 0x34, 0x33, 0x30, 0x4e, 0x39, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x01, 0x71,
};
#endif

void RDNA4Device::buildModeTable() {
	modeCount = 0;
	// The sink's other modes are offered unless rdna4-modeset=0: switching
	// ran on the card for HDMI and DisplayPort boot displays (2026-10-04/06).
	uint32_t ms = 1;
#ifdef AMDRadeonRDNA4_VM_TEST
	uint32_t fake = 1;   // the fixture EDID, unless rdna4-fakeedid=0
#endif
	PE_parse_boot_argn("rdna4-modeset", &ms, sizeof(ms));
	modesetRequested = ms != 0;

#ifdef AMDRadeonRDNA4_VM_TEST
	PE_parse_boot_argn("rdna4-fakeedid", &fake, sizeof(fake));
	if (!edidLen && fake) {
		memcpy(edidData, kVmFixtureEdid, sizeof(kVmFixtureEdid));
		edidLen = sizeof(kVmFixtureEdid);
		FBLOG("modes: VM test build: serving the Lenovo fixture EDID");
	}
#endif

	// Modes reuse the GOP framebuffer's memory and pitch, so none may be
	// larger than it. TMDS stays at 340 MHz or less unless the sink takes
	// HDMI 2.0 scrambling (SCDC and a higher character rate in its EDID), and
	// within 25 % of the boot pixel clock, which the GOP's DISPCLK is known to
	// carry, until DISPCLK is read and raised explicitly.
	Modes::Limits lim {};
	lim.maxHActive = fbWidth;
	lim.maxVActive = fbHeight;
	if (bootTimingValid && (bootTiming.hActive > fbWidth || bootTiming.vActive > fbHeight))
		FBLOG("modes: the console is %ux%u and the firmware stretches it onto its %ux%u timing: nothing larger "
		      "than the console is offered (the bootloader's console resolution decides this)", fbWidth, fbHeight,
		      bootTiming.hActive, bootTiming.vActive);
	const Edid::Hdmi2Caps hdmi2 = Edid::hdmi2Caps(edidData, edidLen);
	lim.maxPixelClockKHz = ModeSet::kScrambleFromKHz;
	if (hdmi2.scdc && hdmi2.maxTmdsKHz > lim.maxPixelClockKHz)
		lim.maxPixelClockKHz = hdmi2.maxTmdsKHz < ModeSet::kMaxTmdsKHz ? hdmi2.maxTmdsKHz : ModeSet::kMaxTmdsKHz;
	if (bootTimingValid && bootTiming.pixelClockKHz && bootTiming.pixelClockKHz * 5 / 4 < lim.maxPixelClockKHz)
		lim.maxPixelClockKHz = bootTiming.pixelClockKHz * 5 / 4;
	// A passive DP++ adapter carries no more than its ID block says; the
	// boot mode stays, the firmware has shown that it gets through.
	const uint32_t adaptorKHz = pipe.valid() ? dpDualMaxKHz(pipe.hpd) : 0;
	if (adaptorKHz) {
		const uint32_t bootKHz = bootTimingValid ? bootTiming.pixelClockKHz : 0;
		const uint32_t capKHz = adaptorKHz > bootKHz ? adaptorKHz : bootKHz;

		if (capKHz < lim.maxPixelClockKHz)
			lim.maxPixelClockKHz = capKHz;
	}

	// DisplayPort has no TMDS limit, but its link is not retrained: what the link carries, and DISPCLK.
	if (pipe.valid() && pipe.signal == Pipe::Signal::DpSst && bootTimingValid && bootTiming.pixelClockKHz) {
		DpTrain::Sink sink = bootDpSink;
		DpTrain::Link link = bootDpLink;
		const bool known = bootDpLinkKnown || readBootDpLink(sink, link);
		bootDpLinkKHz = known ? DpTrain::bandwidthKbps(link) / 24 : 0;
		lim.maxPixelClockKHz = bootDpMaxKHz();
		FBLOG("modes: DisplayPort modes up to %u kHz (link %u lane(s) at 0x%02x: %u kHz at 8 bpc; DISPCLK %u kHz; boot "
		      "%u kHz)", lim.maxPixelClockKHz, link.lanes, link.rate, bootDpLinkKHz, dispclkTargetKHz(),
		      bootTiming.pixelClockKHz);
	}

	// The EDID with its blocks past the first extension (DisplayID timings), for the table alone.
	static uint8_t full[512];
	memcpy(full, edidData, sizeof(edidData));
	memcpy(full + sizeof(edidData), edidMore, sizeof(edidMore));
	const size_t fullLen = edidLen == sizeof(edidData) ? edidLen + edidMoreLen : edidLen;
	if (modesetRequested && edidLen)
		modeCount = Modes::build(full, fullLen, lim, modeTable, Modes::MaxModes);

	Edid::BaseInfo base {};
	if (edidLen && Edid::parseBaseBlock(edidData, edidLen, base)) {
		imageWidthMm  = base.widthMm;
		imageHeightMm = base.heightMm;
	}

	// The live mode: the timing read back from the OTG, adopting the EDID
	// entry it matches (for the exact pixel clock) or added as its own
	// entry. Without a readable pipe (VM, unknown hardware), the EDID's
	// preferred timing stands in when it matches the console geometry.
	int bootIdx = -1;
	if (bootTimingValid && bootTiming.pixelClockKHz) {
		bootIdx = Modes::match(modeTable, modeCount, bootTiming);
		if (bootIdx >= 0)
			bootTiming.pixelClockKHz = modeTable[bootIdx].t.pixelClockKHz;
		else
			bootIdx = Modes::ensure(modeTable, modeCount, Modes::MaxModes, bootTiming,
			                        Modes::SourceBoot);
	} else if (edidLen) {
		Edid::DetailedTiming pref {};
		if (Edid::preferredTiming(edidData, edidLen, pref) &&
		    pref.hActive == fbWidth && pref.vActive == fbHeight) {
			bootIdx = Modes::match(modeTable, modeCount, pref);
			if (bootIdx < 0)
				bootIdx = Modes::ensure(modeTable, modeCount, Modes::MaxModes, pref,
				                        Modes::SourceBoot);
		}
	}
	if (bootIdx < 0) {
		// The live timing is unknown, so no other mode could be switched to
		// safely: keep upstream's single fixed mode.
		modeCount = 0;
		FBLOG("modes: live timing unknown — single fixed %ux%u mode", fbWidth, fbHeight);
		return;
	}

	currentModeId = defaultModeId = modeTable[bootIdx].id;
	liveTiming = modeTable[bootIdx].t;
	liveTimingValid = bootTimingValid;
	for (size_t i = 0; i < modeCount; i++) {
		const Modes::Mode &m = modeTable[i];
		FBLOG("modes: id %u %ux%u@%u.%03u %u kHz%s%s", m.id, m.t.hActive, m.t.vActive,
		      m.refreshMilliHz / 1000, m.refreshMilliHz % 1000, m.t.pixelClockKHz,
		      m.native ? " native" : "", m.id == currentModeId ? " (live)" : "");
	}
	FBLOG("modes: %lu mode(s), switching %s", static_cast<unsigned long>(modeCount),
	      modesetRequested ? "on (rdna4-modeset=0 turns it off)" : "off (rdna4-modeset=0: boot mode only)");
	// The plane's blank-end register as the firmware has it for its own mode,
	// next to what the mode switch would write there for that mode.
	if (bootTimingValid && bootTiming.pixelClockKHz)
		FBLOG("modes: plane blank end: the firmware has 0x%08x, the rule gives 0x%08x for the boot timing",
		      regReadDmu(2, 0x063b + hubpOff()), ModeSet::dlgBlankEnd(bootTiming));
	owner->setProperty("Modes,Count", static_cast<uint64_t>(modeCount), 32);
}

const Modes::Mode *RDNA4Device::findMode(uint32_t id) const {
	for (size_t i = 0; i < modeCount; i++)
		if (modeTable[i].id == id)
			return &modeTable[i];
	return nullptr;
}

// The VBIOS display path wired to an HPD pin (counted from 1, as the lit
// pipe's is).
bool RDNA4Device::pathForHpd(uint8_t hpdPin, AtomBios::DisplayPath &out, uint8_t *ddcLine) {
	AtomBios::DisplayPath paths[AtomBios::MaxDisplayPaths];
	size_t n = atomBios.getDisplayPaths(paths, AtomBios::MaxDisplayPaths);
	for (size_t i = 0; i < n; i++) {
		AtomBios::PathRecords rec;
		if (atomBios.getPathRecords(paths[i], rec) && rec.hasHpd && rec.hpdPin == hpdPin) {
			out = paths[i];
			if (ddcLine)
				*ddcLine = rec.ddcLine;
			return true;
		}
	}
	return false;
}

// Let `frames` frames of an OTG pass, by its frame counter.
bool RDNA4Device::waitFrames(uint32_t frames, uint32_t otgOffset) {
	const uint32_t reg = Pipe::Reg::kOtgFrameCount + otgOffset;
	uint32_t last = regReadDmu(2, reg) & 0xffffff;
	for (uint32_t seen = 0, us = 0; seen < frames; us += 100) {
		if (us > frames * 100000u)                // 100 ms per frame is 10 Hz
			return false;
		IODelay(100);
		uint32_t now = regReadDmu(2, reg) & 0xffffff;
		if (now != last) {
			seen += (now - last) & 0xffffff;
			last = now;
		}
	}
	return true;
}

bool RDNA4Device::runSteps(const ModeSet::Step *steps, size_t count, const Dmub::Cmd *cmds,
                           size_t ncmds, const char *tag, uint32_t otgOffset, const Pipe2::AuxWrite *aux) {
	for (size_t i = 0; i < count; i++) {
		const ModeSet::Step &s = steps[i];
		if (stepTrail) {
			// The boot pipe runs: its OTG0 control register reads 0xffffffff only when the card no longer answers,
			// and the next accesses can hang the machine. Checked after each step (below), 5 ms on (a DMUB command acts
			// after it was posted); each step logged and left as a breadcrumb first.
			char text[96];
			snprintf(text, sizeof(text), "%s step %lu: %.48s", stepTrail, static_cast<unsigned long>(i), s.what ? s.what : "?");
			breadcrumb(text);
			FBLOG("%s: step %lu op %u seg %u dword 0x%04x mask 0x%08x value 0x%08x arg %u: %s", stepTrail,
			      static_cast<unsigned long>(i), static_cast<unsigned>(s.op), s.seg, s.dword, s.mask, s.value, s.arg,
			      s.what ? s.what : "?");
		}
		switch (s.op) {
		case ModeSet::Op::Write:
			regWriteDmu(s.seg, s.dword, s.value);
			break;
		case ModeSet::Op::Update:
			regWriteDmu(s.seg, s.dword, (regReadDmu(s.seg, s.dword) & ~s.mask) | s.value);
			break;
		case ModeSet::Op::Copy:
			regWriteDmu(s.seg, s.dword, regReadDmu(s.seg, s.arg));
			break;
		case ModeSet::Op::Delay:
			IODelay(s.arg);
			break;
		case ModeSet::Op::Scdc:
			// Like amdgpu, carry on if the sink does not answer (in time).
			scdcConfigureWait(static_cast<uint8_t>(s.arg), static_cast<uint8_t>(s.value));
			break;
		case ModeSet::Op::Require: {
			const uint32_t v = regReadDmu(s.seg, s.dword);
			if (v == 0xFFFFFFFF || (v & s.mask) != s.value) {
				FBLOG("%s: step %lu not met (%s): 0x%08x", tag, static_cast<unsigned long>(i), s.what, v);
				return false;
			}
			break;
		}
		case ModeSet::Op::WaitSet:
		case ModeSet::Op::WaitClear:
		case ModeSet::Op::WaitValue: {
			const uint32_t want = s.op == ModeSet::Op::WaitSet ? s.mask
			                    : s.op == ModeSet::Op::WaitClear ? 0 : s.value;
			bool ok = false;
			for (uint32_t us = 0;; us += 10) {
				if ((regReadDmu(s.seg, s.dword) & s.mask) == want) {
					ok = true;
					break;
				}
				if (us >= s.arg)
					break;
				IODelay(10);
			}
			if (!ok) {
				FBLOG("%s: step %lu (%s) timed out%s", tag, static_cast<unsigned long>(i),
				      s.what, s.optional ? ", continuing" : "");
				if (!s.optional)
					return false;
			}
			break;
		}
		case ModeSet::Op::Dmub:
			if (stepTrail && s.arg < ncmds)
				FBLOG("%s: step %lu dmub %08x %08x %08x %08x %08x %08x", stepTrail, static_cast<unsigned long>(i),
				      cmds[s.arg][0], cmds[s.arg][1], cmds[s.arg][2], cmds[s.arg][3], cmds[s.arg][4], cmds[s.arg][5]);
			if (s.arg >= ncmds || !dmubSubmit(&cmds[s.arg], 1, tag)) {
				FBLOG("%s: step %lu (%s): DMUB did not take the command", tag,
				      static_cast<unsigned long>(i), s.what);
				return false;
			}
			break;
		case ModeSet::Op::Aux:
			// Like amdgpu, carry on if the sink does not take it.
			if (!aux || !dpcdWrite(xp().aux, aux[s.arg].address, aux[s.arg].data, aux[s.arg].len))
				FBLOG("%s: step %lu (%s): the sink did not take the AUX write, continuing", tag,
				      static_cast<unsigned long>(i), s.what);
			break;
		case ModeSet::Op::Train:
			if (!trainSecondLink()) {
				FBLOG("%s: step %lu (%s): the link did not train", tag, static_cast<unsigned long>(i), s.what);
				return false;
			}
			break;
		case ModeSet::Op::WaitFrames:
			if (!waitFrames(s.arg, otgOffset)) {
				FBLOG("%s: step %lu (%s): the OTG is not counting frames", tag,
				      static_cast<unsigned long>(i), s.what);
				return false;
			}
			break;
		}
		if (stepTrail) {
			IODelay(5000);
			if (regReadDmu(2, Pipe::Reg::kOtgControl) == 0xFFFFFFFF) {
				char text[96];
				snprintf(text, sizeof(text), "%s: the card stopped answering after step %lu", stepTrail,
				         static_cast<unsigned long>(i));
				breadcrumb(text);
				FBLOG("%s: %s (%s); stopped", tag, text, s.what ? s.what : "?");
				return false;
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// DisplayPort link training on the boot display
// ---------------------------------------------------------------------------

bool RDNA4Device::bootSinkPresent() {
	const uint32_t hpdY = regReadDmu(2, 0x28f7);   // DC_GPIO_HPD_Y: one byte per pin, HPD1 lowest
	return hpdY != 0xFFFFFFFF && pipe.hpd >= 1 && pipe.hpd <= 4 && ((hpdY >> (8 * (pipe.hpd - 1))) & 1);
}

uint32_t RDNA4Device::bootDpMaxKHz() {
	const uint32_t boot = bootTimingValid ? bootTiming.pixelClockKHz : 0, dispclk = dispclkTargetKHz();
	if (!bootDpLinkKHz || !dispclk)
		return boot;
	const uint32_t most = bootDpLinkKHz < dispclk ? bootDpLinkKHz : dispclk;
	return most > boot ? most : boot;
}

void RDNA4Device::noteBootDpLink() {
	bootDpLinkKnown = readBootDpLink(bootDpSink, bootDpLink);
}

// The boot display's sink capabilities and the link the firmware trained (DPCD reads): whether that is a link the
// sink says it has.
bool RDNA4Device::readBootDpLink(DpTrain::Sink &sink, DpTrain::Link &link) {
	if (!pipe.valid() || pipe.signal != Pipe::Signal::DpSst || !sinkAuxValid) {
		FBLOG("dptrain: the boot display is not on DisplayPort: nothing to retrain");
		return false;
	}
	// Receiver capability, then what the firmware told the sink: LINK_BW_SET,
	// LANE_COUNT_SET, and from 0x202 on how the lanes are doing.
	uint8_t caps[16] {}, set[2] {}, status[6] {};
	if (!dpcdRead(sinkAuxInst, 0x000, caps, sizeof(caps)) || !dpcdRead(sinkAuxInst, 0x100, set, sizeof(set)) ||
	    !dpcdRead(sinkAuxInst, 0x202, status, sizeof(status))) {
		FBLOG("dptrain: the sink's DPCD did not answer on AUX%u", sinkAuxInst);
		return false;
	}
	const bool parsed = DpTrain::parseCaps(caps, sizeof(caps), sink);
	link = DpTrain::Link { static_cast<uint8_t>(set[1] & 0x1f), set[0] };
	FBLOG("dptrain: sink DPCD %u.%u, up to %u lane(s) at rate 0x%02x%s%s, reading interval 0x%02x; the firmware's "
	      "link: %u lane(s) at rate 0x%02x (LANE_COUNT_SET 0x%02x); lanes %02x %02x, aligned %u", caps[0] >> 4,
	      caps[0] & 0xf, sink.maxLanes, sink.maxRate, sink.tps3 ? ", TPS3" : "",
	      sink.tps4 ? ", TPS4" : "", caps[0x0e], link.lanes, link.rate, set[1], status[0],
	      status[1], status[2] & 1);
	const bool lanes = link.lanes == 1 || link.lanes == 2 || link.lanes == 4;
	const bool known = parsed && lanes && link.lanes <= sink.maxLanes && link.rate >= DpTrain::kRbr &&
	                   link.rate <= sink.maxRate;
	if (!known)
		FBLOG("dptrain: that is not a link this sink says it has: it will not be retrained");
	return known;
}

// DpTrain on one link of this card: the sink's DPCD over AUX channel `aux`,
// and DpPhy's register steps and DMUB commands for link encoder `link`, whose
// connector has hot-plug pin `hpd` and VBIOS object `conn`.
// "no clock recovery, trained": what each attempt of a training ended in.
static const char *triesText(const DpTrain::Report &rep, char *out, size_t cap) {
	out[0] = 0;
	for (uint8_t i = 0; i < rep.attempts && i < DpTrain::kAttempts; i++) {
		if (i)
			strlcat(out, ", ", cap);
		strlcat(out, DpTrain::resultName(rep.tries[i]), cap);
	}
	return out;
}

DpTrain::Result RDNA4Device::trainLink(uint8_t aux, uint8_t link, uint8_t hpd, uint8_t conn, const DpTrain::Sink &sink,
                                       const DpTrain::Link &settings, bool offFirst, DpTrain::Report &rep) {
	struct Ctx {
		RDNA4Device  *dev;
		uint8_t       aux, link, hpd, conn;
		DpTrain::Link settings;
		bool steps(const ModeSet::Step *s, size_t n) { return dev->runSteps(s, n, nullptr, 0, "dptrain", dev->otgOff()); }
		bool dmub(const Dmub::Cmd &cmd) { return dev->dmubSubmit(&cmd, 1, "dptrain"); }
	} ctx { this, aux, link, hpd, conn, settings };
	DpTrain::Io io {};
	io.ctx = &ctx;
	io.read = [](void *c, uint32_t address, uint8_t *data, size_t len) {
		auto *x = static_cast<Ctx *>(c);
		return x->dev->dpcdRead(x->aux, address, data, len);
	};
	io.write = [](void *c, uint32_t address, const uint8_t *data, size_t len) {
		auto *x = static_cast<Ctx *>(c);
		return x->dev->dpcdWrite(x->aux, address, data, len);
	};
	io.phyOn = [](void *c, const DpTrain::Link &l) {
		auto *x = static_cast<Ctx *>(c);
		ModeSet::Step s[DpPhy::kMaxSteps];
		Dmub::Cmd cmd;
		DpPhy::buildEnable(cmd, x->link, x->hpd, l);
		return x->steps(s, DpPhy::configureSteps(x->link, l.lanes, s)) && x->dmub(cmd);
	};
	io.phyOff = [](void *c) {
		auto *x = static_cast<Ctx *>(c);
		ModeSet::Step s[DpPhy::kMaxSteps];
		Dmub::Cmd cmd;
		DpPhy::buildDisable(cmd, x->link, x->hpd, x->conn);
		return x->dmub(cmd) && x->steps(s, DpPhy::offSteps(x->link, s));
	};
	io.pattern = [](void *c, DpTrain::Pattern p) {
		auto *x = static_cast<Ctx *>(c);
		ModeSet::Step s[DpPhy::kMaxSteps];
		return x->steps(s, DpPhy::patternSteps(x->link, p, s));
	};
	io.drive = [](void *c, uint8_t, uint8_t swing, uint8_t preEmphasis) {
		auto *x = static_cast<Ctx *>(c);
		Dmub::Cmd cmd;
		DpPhy::buildDrive(cmd, x->link, x->hpd, x->conn, x->settings, swing, preEmphasis);
		return x->dmub(cmd);
	};
	io.delayUs = [](void *, uint32_t us) {
		if (us >= 10000)
			IOSleep(us / 1000);
		else
			IODelay(us);
	};
	if (offFirst && !io.phyOff(&ctx))
		return rep.result = DpTrain::Result::Io;
	return DpTrain::bringUp(io, sink, settings, &rep);
}

// The boot display's link as the sink and the link encoder have it, in one
// line: to see what a retrain left and whether anything changed it afterwards.
void RDNA4Device::logBootLink(const char *when) {
	uint8_t set[2] {}, status[3] {};
	const bool sink = dpcdRead(sinkAuxInst, 0x100, set, sizeof(set)) && dpcdRead(sinkAuxInst, 0x202, status, sizeof(status));
	const uint32_t dp = pipe.link * 0x124u, arb = regReadDmu(2, auxDword(sinkAuxInst, kAuxArbControl));
	FBLOG("dptrain: %s: sink %s rate 0x%02x lanes 0x%02x status %02x %02x %02x; DP_VID_STREAM_CNTL 0x%08x "
	      "DP_LINK_CNTL 0x%08x DP_CONFIG 0x%08x pattern 0x%08x; AUX arbitration 0x%08x", when,
	      sink ? "answers:" : "does not answer:", set[0], set[1], status[0], status[1], status[2],
	      regReadDmu(2, 0x2122 + digOff()), regReadDmu(2, 0x211e + dp), regReadDmu(2, 0x2121 + dp),
	      regReadDmu(2, 0x212e + dp), arb);
	// What ties the stream encoder to the link, and what a reset of it would lose.
	FBLOG("dptrain: %s: SYMCLK 0x%08x BE_CLK 0x%08x BE_EN 0x%08x BE_CNTL 0x%08x FE_CLK 0x%08x FE_EN 0x%08x FIFO 0x%08x "
	      "mapper 0x%08x; DP_PIXEL_FORMAT 0x%08x MSA_MISC 0x%08x DP_VID_TIMING 0x%08x DP_STEER_FIFO 0x%08x", when,
	      regReadDmu(1, 0x00a0 + pipe.dig), regReadDmu(2, 0x20bb + dp), regReadDmu(2, 0x20bd + dp),
	      regReadDmu(2, 0x20bc + dp), regReadDmu(2, 0x2094 + digOff()), regReadDmu(2, 0x2095 + digOff()),
	      regReadDmu(2, 0x209b + digOff()), regReadDmu(2, 0x1f0d + pipe.dig), regReadDmu(2, 0x211f + digOff()),
	      regReadDmu(2, 0x2124 + digOff()), regReadDmu(2, 0x2126 + digOff()), regReadDmu(2, 0x2123 + digOff()));
}

DpTrain::Result RDNA4Device::retrainBootLink(const char *why) {
	AtomBios::DisplayPath path {};
	if (!bootDpLinkKnown || !pathForHpd(pipe.hpd, path)) {
		FBLOG("dptrain: %s: not retrained: the firmware's link is not known", why);
		return DpTrain::Result::BadLink;
	}
	// The stream stops feeding the link while it is down, as for display
	// sleep, and the transmitter goes off before it is brought up again.
	const uint32_t kDpVidStreamCntl = 0x2122 + digOff();
	const uint32_t stream = regReadDmu(2, kDpVidStreamCntl);
	FBLOG("dptrain: %s: retraining %u lane(s) at rate 0x%02x on link %u (AUX%u, HPD%u), stream 0x%08x", why,
	      bootDpLink.lanes, bootDpLink.rate, pipe.link, sinkAuxInst, pipe.hpd, stream);
	logBootLink("before");
	regWriteDmu(2, kDpVidStreamCntl, stream & ~1u);
	DpTrain::Report rep {};
	const DpTrain::Result r = trainLink(sinkAuxInst, pipe.link, pipe.hpd, static_cast<uint8_t>(path.connectorObjId & 0xff),
	                                    bootDpSink, bootDpLink, true, rep);
	// The video back, unless display sleep had it off. With the transmitter
	// having been off, the stream only starts again if the stream encoder is
	// first tied to the link, as amdgpu does between training and unblank, and
	// then started the way a mode switch starts it (card, 2026-10-06: without
	// either the link trained and the screen stayed black).
	if (r == DpTrain::Result::Ok && (stream & 1)) {
		ModeSet::Step s[DpPhy::kMaxSteps];
		runSteps(s, DpPhy::streamSteps(pipe.dig, pipe.link, s), nullptr, 0, "dptrain", otgOff());
		if (applyMode(Modes::Mode { currentModeId, liveTiming, liveTiming.refreshMilliHz(), false, Modes::SourceBoot }) !=
		    kIOReturnSuccess)
			regWriteDmu(2, kDpVidStreamCntl, stream);
	}
	char tries[160];
	FBLOG("dptrain: %s after %u attempt(s) (%s): swing %u, pre-emphasis %u, lanes %02x %02x, aligned %u; stream 0x%08x",
	      DpTrain::resultName(r), rep.attempts, triesText(rep, tries, sizeof(tries)), rep.swing, rep.preEmphasis,
	      rep.status[0], rep.status[1], rep.status[2] & 1, regReadDmu(2, kDpVidStreamCntl));
	logBootLink("after");
	return r;
}

// The console's address as the display hub counts it: the memory controller's VRAM base (the MM hub's
// MC_VM_FB_LOCATION_BASE, [23:0] = MC address >> 24, where amdgpu takes its fb_start) plus the console's place in
// VRAM (its CPU address less BAR0's). What amdgpu puts in a plane's address register for a buffer in VRAM.
bool RDNA4Device::consoleMcAddress(uint64_t &mc) {
	constexpr uint32_t kMmFbLocationBase = 0x0554;   // MMMC_VM_FB_LOCATION_BASE (gfxregs.hpp MmFbLocationBase)
	uint32_t off = 0;
	IODeviceMemory *bar0 = pciDevice ? pciDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0) : nullptr;
	if (!bar0 || !rmmio || !ipDiscovery.regByteOffset(IpDiscovery::HwMmhub, 0, 0, kMmFbLocationBase, off) ||
	    off + 4 > rmmioSize)
		return false;
	const uint64_t base = static_cast<uint64_t>(rmmio[off / 4] & 0xffffff) << 24;
	const uint64_t bar = bar0->getPhysicalAddress();
	if (!base || fbPhysBase < bar || fbPhysBase >= bar + bar0->getLength())
		return false;
	mc = base + (fbPhysBase - bar);
	return true;
}

// The selected extra pipe down as a power loss leaves it: asleep by its table, its timing generator stopped, an HDMI
// one's link off. Not lit any more.
void RDNA4Device::takeExtraDown() {
	static ModeSet::Step otgStop[16];
	size_t notg = 0;
	auto crumb = [this](const char *what) {
		char step[64];
		snprintf(step, sizeof(step), "cold start: extra pipe %u %s", xp().pipe, what);
		breadcrumb(step);
	};
	crumb(xp().on ? "asleep" : "off");
	if (xp().on)
		runSecondPipePart(Pipe2::Part::Sleep);
	crumb("its OTG stopped");
	// Stopped while its link still clocks it: an HDMI timing generator runs on its link's clock, and with the
	// transmitter off it never reaches the end of its frame to stop (card, 2026-11-15: OTG1_OTG_CLOCK_CONTROL timed
	// out).
	if (!DcnInit::steps(otgStop, 16, notg, DcnInit::Table::OtgOff, xp().pipe) ||
	    !runSteps(otgStop, notg, nullptr, 0, "dcninit: extra otg off", xp().pipe * Pipe::Reg::kOtgStride))
		FBLOG("dcninit: extra pipe %u's timing generator did not stop", xp().pipe);
	// HDMI: its sleep leaves the transmitter and the link encoder on. Off as Linux's warm-card init turns an enabled
	// DIG off (dce110_power_down: disable_output, the transmitter disable), the back end as releaseBootPipe takes it
	// (the firmware leaves DIG_BE_ENABLE set).
	const Pipe2::Config &c = Pipe2::config();
	AtomBios::DisplayPath xpath {};
	crumb("its link off");
	if (!c.dp && pathForHpd(c.hpd, xpath)) {
		Dmub::Cmd cmd;
		DpPhy::buildDisable(cmd, c.link, c.hpd, static_cast<uint8_t>(xpath.connectorObjId & 0xff));
		const ModeSet::Step off[] = {
			{ ModeSet::Op::Dmub, 0, false, 0, 0, 0, 0, "DIG1_TRANSMITTER_CONTROL disable" },
			{ ModeSet::Op::Update, 2, false, Pipe::Reg::kDigBeEnCntl + c.dig * Pipe::Reg::kDigStride, 0x1, 0x0, 0,
			  "DIG_BE_ENABLE: the link encoder off" },
		};
		if (!runSteps(off, 2, &cmd, 1, "dcninit: extra off", c.pipe * Pipe::Reg::kOtgStride))
			FBLOG("dcninit: extra pipe %u's link %u did not go off cleanly", c.pipe, c.link);
	} else if (c.dp) {
		// DisplayPort: the link encoder off too (lighting requires it), the transmitter left on (pipe2.cpp 'O': its
		// disable stopped the card on link 1; lighting enables a transmitter that is on, card-verified).
		const ModeSet::Step off[] = {
			{ ModeSet::Op::Update, 2, false, Pipe::Reg::kDigBeEnCntl + c.link * Pipe::Reg::kDigStride, 0x1, 0x0, 0,
			  "DIG_BE_ENABLE: the link encoder off" },
		};
		(void)runSteps(off, 1, nullptr, 0, "dcninit: extra off", c.pipe * Pipe::Reg::kOtgStride);
	}
	xp().lit = xp().on = false;
}

bool RDNA4Device::systemSleepWanted() {
	// On since 2026-11-15: system sleep and wake ran on the card with one and with two displays, the desktop back each time.
#ifdef AMDRadeonRDNA4_VM_TEST
	uint32_t on = 0;
#else
	uint32_t on = 1;
#endif
	PE_parse_boot_argn("rdna4-syssleep", &on, sizeof(on));
	return on != 0;
}

void RDNA4Device::head2SleepTest(int what) {
	ExtraHold hold(*this);
	selectExtra(0);
	useSlotTable();
	const Pipe2::Config &c = Pipe2::config();
	FBLOG("head2sleep: %s the second display's pipe %u (%s, lit %d, on %d)", what == 2 ? "waking" : "putting to sleep",
	      xp().pipe, c.dp ? "DisplayPort" : "HDMI", xp().lit, xp().on);
	if (!xp().lit)
		return;
	stepTrail = what == 2 ? "head2sleep: wake" : "head2sleep: sleep";
	setSecondPipePower(what == 2);
	stepTrail = nullptr;
	// A clean end in the trail ("stopped ..."): a breadcrumb left there would make the next boot think this one hung
	// and skip its bring-up (no desktop).
	char text[64];
	snprintf(text, sizeof(text), "stopped: head2sleep %s done, on %d", what == 2 ? "wake" : "sleep", xp().on);
	if (compute)
		compute->trail(text);
	FBLOG("%s", text);
}

void RDNA4Device::breadcrumb(const char *step) {
	if (!compute || !rmmio)
		return;
	uint32_t otgs = 0;
	for (uint32_t i = 0; i < 4; i++)
		otgs |= (regReadDmu(2, Pipe::Reg::kOtgControl + i * Pipe::Reg::kOtgStride) & 1u) << i;
	char text[96];
	snprintf(text, sizeof(text), "s3: %s (OTGs on 0x%x)", step, otgs);
	compute->trail(text);
}

void RDNA4Device::extraAuxPadsAfterPowerLoss() {
	if (!coldBoot || !rmmio || !Pipe2::config().dp)
		return;
	const uint8_t aux = xp().aux;
	const uint32_t mask = kGpioDdcMask + 4u * aux, was = regReadDmu(2, mask);
	ddcPadMode(aux, false);
	FBLOG("dcninit: extra pipe %u: AUX%u pads: DDC%u_MASK 0x%08x -> 0x%08x", xp().pipe, aux, aux + 1u, was, regReadDmu(2, mask));
}

bool RDNA4Device::coldStart(uint32_t &relight) {
	uint32_t on = 0;
	relight = 0;
	if ((!Trial::option("rdna4-dcninit", on) || !on) && !coldBoot)
		return false;
	ExtraHold hold(*this);
	if (!uhdClocks) {   // a cold start's order: the 4K boot display lit on the kext's clocks, not the GOP's
		FBLOG("dcninit: not run: the display clocks are not raised (no SMU, or a hard minimum not met)");
		return false;
	}
	AtomBios::DisplayPath path {};
	if (!bootDpLinkKnown)   // read with rdna4-dptrain only: the sink's capabilities and the firmware's link (DPCD reads)
		noteBootDpLink();
	// The live pixel clock is measured from the frame period (533246 kHz on one boot, 533250 on the next): the
	// timing's shape has to be the table's, its clock within 0.05 %.
	auto nearTiming = [](Edid::DetailedTiming a, const Edid::DetailedTiming &b) {
		const uint32_t diff = a.pixelClockKHz > b.pixelClockKHz ? a.pixelClockKHz - b.pixelClockKHz
		                                                        : b.pixelClockKHz - a.pixelClockKHz;
		a.pixelClockKHz = b.pixelClockKHz;
		return Edid::sameTiming(a, b) && diff <= b.pixelClockKHz / 2000;
	};
	// The one boot pipe there is a table for: the GOP's DisplayPort display on pipe 0 at its 4K timing. After a power
	// loss it may have been switched to another mode since (2560x1440 on the native desktop, 2026-11-22: the cold start
	// refused, neither display came back from sleep): lit at the table's timing, then switched back to that mode below.
	const Edid::DetailedTiming wanted = liveTiming;
	if (!pipe.valid() || pipe.otg != 0 || pipe.hubp != 0 || pipe.dig != 0 || pipe.link != 0 || pipe.hpd != 1 ||
	    !bootDpLinkKnown || !pathForHpd(pipe.hpd, path) || !Pipe2::use(1, false, 0, true) ||
	    (!coldBoot && !nearTiming(liveTiming, Pipe2::config().timing))) {
		FBLOG("dcninit: not run: the boot pipe is not the DisplayPort one on pipe 0 at the table's 3840x2160 "
		      "(OTG%u HUBP%u DIG%u link %u HPD%u, %ux%u at %u kHz, link known %d)", pipe.otg, pipe.hubp, pipe.dig,
		      pipe.link, pipe.hpd, liveTiming.hActive, liveTiming.vActive, liveTiming.pixelClockKHz, bootDpLinkKnown);
		useSlotTable();   // the slot's own table again
		return false;
	}
	// (read now: the slot's own table is selected again before the lighting is checked below)
	const Edid::DetailedTiming table = Pipe2::config().timing;
	// The console's address as the display hub counts it, computed (as after a power loss, when no pipe holds it);
	// on this warm card it has to be the address the GOP's HUBP0 scans.
	const uint64_t gop = coldBoot ? coldGopSurface : bootScanout();
	uint64_t surface = 0;
	if (!consoleMcAddress(surface) || surface != gop) {
		FBLOG("dcninit: not run: the console's computed address 0x%llx is not the GOP's 0x%llx", surface, gop);
		useSlotTable();
		return false;
	}
	// The DCHUB global timer, started by the kext from the VBIOS's reference clock (nothing else starts it after a
	// power loss); on this warm card the GOP's has to be running with the divider the kext computes.
	uint32_t refKHz = 0;
	const uint32_t gopTimer = coldBoot ? coldGopTimer : regReadDmu(2, DcnInit::kTimerCntl);
	ModeSet::Step timerOn[2];
	size_t ntimer = 0;
	if (!atomBios.getDceRefClock(refKHz) || !DcnInit::timerSteps(refKHz, timerOn, 2, ntimer) ||
	    !(gopTimer & DcnInit::kTimerEnable) || (gopTimer & DcnInit::kTimerRefDivMask) != DcnInit::timerRefDiv(refKHz)) {
		FBLOG("dcninit: not run: the global timer the GOP left (0x%08x) is not the one the kext starts (reference "
		      "%u kHz, divider %u)", gopTimer, refKHz, DcnInit::timerRefDiv(refKHz));
		useSlotTable();
		return false;
	}
	const uint8_t connector = static_cast<uint8_t>(path.connectorObjId & 0xff);

	// 0. The lit extra pipes taken down (the init blanks and gates every pipe), lit again after. After a power loss a
	// DisplayPort one has nothing left running: only marked dark.
	breadcrumb("cold start: extra pipes down");
	for (size_t i = 0; i < kMaxExtra; i++) {
		selectExtra(i);
		useSlotTable();
		if (!xp().lit)
			continue;
		if (Pipe2::config().dp && coldBoot)
			xp().lit = xp().on = false;
		else
			takeExtraDown();
		relight |= 1u << i;
	}
	selectExtra(0);
	(void)Pipe2::use(1, false, 0, true);   // the boot pipe's table again (checked above)
	if (relight)
		FBLOG("dcninit: extra pipe(s) 0x%x asleep, to be lit again after", relight);

	ExtraPipe boot {};
	boot.pipe = 0;
	boot.hpd = pipe.hpd;
	boot.aux = sinkAuxInst;
	boot.sink = bootDpSink;
	boot.link = bootDpLink;
	boot.target.connectorObjId = connector;
	xpOverride = &boot;

	Pipe2::Target t {};
	t.litHubp = 0;
	t.alone = true;   // its VM aperture left as the hardware has it, as amdgpu leaves it on a discrete card
	t.encoderObjId = path.encoderObjId;
	t.connectorObjId = connector;
	t.surface = surface;
	t.depth = Pipe2::Depth::Plane;
	t.linkRate = bootDpLink.rate;
	auto *plan = static_cast<Pipe2::Plan *>(IOMalloc(sizeof(Pipe2::Plan)));
	static ModeSet::Step init[256], otgStop[16];
	size_t ninit = 0, notg = 0;
	const char *why = "";
	bool ok = plan && DcnInit::steps(init, 256, ninit);
	FBLOG("dcninit: boot pipe 0 at 0x%llx (computed, as the GOP's), link %u lane(s) at 0x%02x on AUX%u: taking it "
	      "down, %lu steps of dcn401_init_hw, lighting it again", surface, bootDpLink.lanes, bootDpLink.rate,
	      sinkAuxInst, static_cast<unsigned long>(ninit));
	// 1. The stream and the link off, as Linux's link_set_dpms_off (the table's sleep part). 2. The firmware's
	// timing generator stopped. Also after the reset (coldBoot): a mode-1 reset leaves the timing generators
	// running (card, 2026-11-15: OTG0 at 59.997 Hz, its links dark).
	t.part = Pipe2::Part::Sleep;
	breadcrumb("cold start: boot pipe down");
	ok = ok && Pipe2::build(t, *plan, &why) &&
	     runSteps(plan->steps, plan->count, plan->cmds, plan->ncmds, "dcninit: down", otgOff(), plan->aux);
	if (ok)
		breadcrumb("cold start: boot OTG off");
	ok = ok && DcnInit::steps(otgStop, 16, notg, DcnInit::Table::OtgOff) &&
	     runSteps(otgStop, notg, nullptr, 0, "dcninit: otg off", otgOff());
	// 2b. The global timer stopped, as after a power loss, and started by the kext.
	if (ok)
		regWriteDmu(2, DcnInit::kTimerCntl, gopTimer & ~DcnInit::kTimerEnable);
	ok = ok && runSteps(timerOn, ntimer, nullptr, 0, "dcninit: timer", otgOff());
	if (ok)
		FBLOG("dcninit: global timer 0x%08x stopped, started again at %u kHz / %u: 0x%08x", gopTimer, refKHz,
		      DcnInit::timerRefDiv(refKHz), regReadDmu(2, DcnInit::kTimerCntl));
	// 3. Every pipe blanked and power gated, and the rest dcn401_init_hw does.
	if (ok)
		breadcrumb("cold start: dcn401_init_hw");
	ok = ok && runSteps(init, ninit, nullptr, 0, "dcninit: init", otgOff());
	// 4. The IP request window open, as the lighting plans require (as the GOP leaves it).
	if (ok)
		regWriteDmu(2, 0x00a0, 1);
	// 4b. After a power loss the boot display's DDC pads are back in their reset mode: AUX mode, as Linux's
	// hw_ddc.c sets them each time the AUX engine is taken (the GOP had; a mode-1 reset leaves them as they were).
	if (ok && coldBoot) {
		const uint32_t mask = kGpioDdcMask + 4u * sinkAuxInst, was = regReadDmu(2, mask), ctrl5 = regReadDmu(2, kGpioAuxCtrl5);
		ddcPadMode(sinkAuxInst, false);
		FBLOG("dcninit: AUX%u pads: DDC%u_MASK 0x%08x -> 0x%08x, AUX_CTRL_5 0x%08x -> 0x%08x", sinkAuxInst, sinkAuxInst + 1u,
		      was, regReadDmu(2, mask), ctrl5, regReadDmu(2, kGpioAuxCtrl5));
	}
	// 5. Pipe 0 lit again, its link trained by the kext.
	if (ok)
		breadcrumb("cold start: boot pipe light");
	t.part = Pipe2::Part::Light;
	ok = ok && Pipe2::build(t, *plan, &why) &&
	     runSteps(plan->steps, plan->count, plan->cmds, plan->ncmds, "dcninit: light", otgOff(), plan->aux);
	xpOverride = nullptr;
	if (plan)
		IOFree(plan, sizeof(Pipe2::Plan));
	useSlotTable();
	const uint64_t periodNs = measureFramePeriodNs(otgOff());
	const uint32_t mHz = periodNs ? static_cast<uint32_t>(1000000000000ULL / periodNs) : 0;
	FBLOG("dcninit: %s%s%s; OTG0 measured %u.%03u Hz, HUBP0_DCHUBP_CNTL 0x%08x", ok ? "boot pipe lit again" : "FAILED",
	      *why ? ": " : "", why, mHz / 1000, mHz % 1000, regReadDmu(2, 0x05f4));
	// Lit at the table's timing, with the table's request timing: the mode it ran before the power loss set again.
	if (ok && !nearTiming(wanted, table)) {
		liveTiming = reqTiming = table;
		liveTimingValid = reqTimingValid = true;
		const IOReturn back = applyMode(Modes::Mode { currentModeId, wanted, wanted.refreshMilliHz(), false, Modes::SourceBoot });
		FBLOG("dcninit: the mode before the power loss (%ux%u at %u kHz) set again: %s (0x%x)", wanted.hActive, wanted.vActive,
		      wanted.pixelClockKHz, back == kIOReturnSuccess ? "done" : "FAILED, the table's timing kept", back);
	}
	return ok;
}

bool RDNA4Device::bootPipeTarget(ModeSet::Target &t) {
	AtomBios::DisplayPath path {};
	uint8_t ddcLine = 0;
	if (!pathForHpd(pipe.hpd, path, &ddcLine))
		return false;
	t.sinkScdc = Edid::hdmi2Caps(edidData, edidLen).scdc;
	t.ddcLine = ddcLine;
	t.otg = pipe.otg;
	t.dig = pipe.dig;
	t.link = pipe.link;
	t.hpd = pipe.hpd;
	t.opp = pipe.opp;
	t.hubp = pipe.hubp;
	t.encoderObjId = path.encoderObjId;
	t.connectorObjId = path.connectorObjId;
	// DSCL_AUTOCAL.AUTOCAL_MODE: the firmware stretching a smaller console.
	const uint32_t autocal = regReadDmu(2, 0x0d19 + dppOff());
	t.scalerOff = autocal != 0xFFFFFFFF && (autocal & 0x3) != 0;
	return true;
}

IOReturn RDNA4Device::applyMode(const Modes::Mode &m) {
	if (bootReleased) {
		FBLOG("modes: switch to id %u refused: the boot pipe is released, its connector is a head's", m.id);
		return kIOReturnUnsupported;
	}
	const bool dp = pipe.signal == Pipe::Signal::DpSst;
	if (!pipe.valid() || !(pipe.isTmds() || dp) || !liveTimingValid || !rmmio || !ipDiscovery.isValid()) {
		FBLOG("modes: switch to id %u refused: no programmable HDMI or DisplayPort pipe", m.id);
		return kIOReturnUnsupported;
	}
	// The plane scans the console's memory: a mode larger than that (the
	// firmware's own timing over a console it stretches) would scan past it.
	if (m.t.hActive > fbWidth || m.t.vActive > fbHeight) {
		FBLOG("modes: switch to id %u refused: %ux%u does not fit the %ux%u console", m.id, m.t.hActive, m.t.vActive,
		      fbWidth, fbHeight);
		return kIOReturnUnsupported;
	}
	ModeSet::Target t {};
	if (!bootPipeTarget(t)) {
		FBLOG("modes: switch to id %u refused: no VBIOS path for HPD%u", m.id, pipe.hpd);
		return kIOReturnUnsupported;
	}

	// DisplayPort: the link stays as the firmware trained it and only the
	// stream is retimed, so the pixel clock comes from the DTO running now,
	// scaled. Both it and DP_VID_M are read once, before anything changes.
	ModeSet::DpDto dtoNow {};
	uint32_t vidMNow = 0;
	const uint32_t dtoFromKHz = liveTiming.pixelClockKHz;
	if (dp) {
		constexpr uint32_t kDpVidM = 0x2128;
		uint32_t cntl = 0;
		if (!readDpDto(pipe.otg, dtoNow, &cntl)) {
			FBLOG("modes: switch to id %u refused: OTG%u's pixel clock is not the DP DTO (0x%08x)", m.id,
			      pipe.otg, cntl);
			return kIOReturnUnsupported;
		}
		vidMNow = regReadDmu(2, kDpVidM + digOff()) & 0xffffff;
		t.dp = true;
		// Above the boot clock only on the kext's DISPCLK (raised when the firmware runs), not the GOP's.
		t.dpMaxKHz = uhdClocks ? bootDpMaxKHz() : bootTiming.pixelClockKHz;
	}
	// The plane's request timing (HUBP DLG/TTU, DET) for the mode: Linux's DML values from the boot display's table
	// (pipe 0 on the first DisplayPort connector), as the second pipes take theirs. Without them the plane keeps the
	// request timing it has, which is the GOP's boot mode's until a switch with values replaced it: a mode that has to
	// deliver its lines faster than that underflows (2560x1440@120 on the 4K mode's, card, 2026-11-15), refused.
	static ModeSet::Step modeRegs[Pipe2::kMaxModeSteps];
	ExtraHold hold(*this);
	struct TableBack {
		RDNA4Device &dev;
		bool used;
		~TableBack() { if (used) dev.useSlotTable(); }
	} back { *this, dp && pipe.otg == 0 && pipe.hubp == 0 && pipe.dig == 0 && pipe.hpd == 1 && Pipe2::use(1, false, 0, true) };
	const bool known = back.used && Pipe2::modeKnown(m.t);
	if (!reqTimingValid) {
		reqTiming = bootTiming;
		reqTimingValid = bootTimingValid;
	}
	const auto lineFaster = [](const Edid::DetailedTiming &a, const Edid::DetailedTiming &b) {   // a's lines in less time
		return static_cast<uint64_t>(a.hActive) * b.pixelClockKHz < static_cast<uint64_t>(b.hActive) * a.pixelClockKHz;
	};
	if (dp && !known && (!reqTimingValid || lineFaster(m.t, reqTiming))) {
		FBLOG("modes: switch to id %u refused: no request timing from Linux for %ux%u at %u kHz, and the plane's (of "
		      "%ux%u at %u kHz) does not deliver its lines fast enough", m.id, m.t.hActive, m.t.vActive,
		      m.t.pixelClockKHz, reqTiming.hActive, reqTiming.vActive, reqTiming.pixelClockKHz);
		return kIOReturnUnsupported;
	}
	auto aim = [&](const Edid::DetailedTiming &from, const Edid::DetailedTiming &to) {
		t.from = from;
		t.to = to;
		t.extra = modeRegs;
		t.nextra = 0;
		if (back.used && Pipe2::modeKnown(to)) {
			t.nextra = Pipe2::modeSteps(to, modeRegs, Pipe2::kMaxModeSteps);
			clampDet(modeRegs, t.nextra, secondPipeDet(to));
		}
		if (!dp)
			return;
		if (!ModeSet::scaleDpDto(dtoNow, dtoFromKHz, to.pixelClockKHz, t.dto))
			t.dto = {};
		// M / 0x8000 = pixel clock / link clock; the hardware measures it once the
		// stream runs. Seed it from the running value, or as on four HBR3 lanes.
		t.vidM = vidMNow ? static_cast<uint32_t>(static_cast<uint64_t>(vidMNow) * to.pixelClockKHz / dtoFromKHz)
		                 : static_cast<uint32_t>(0x8000ull * to.pixelClockKHz / 810000);
	};
	aim(liveTiming, m.t);
	const char *why = "";
	if (!ModeSet::build(t, modePlan, &why)) {
		FBLOG("modes: switch to id %u refused: %s", m.id, why);
		return kIOReturnUnsupported;
	}
	FBLOG("modes: switching to id %u %ux%u@%u.%03u (%u kHz, vstartup %u): %lu steps, "
	      "%lu DMUB commands, request timing %s", m.id, m.t.hActive, m.t.vActive, m.refreshMilliHz / 1000,
	      m.refreshMilliHz % 1000, m.t.pixelClockKHz, ModeSet::vstartupLines(m.t),
	      static_cast<unsigned long>(modePlan.count), static_cast<unsigned long>(modePlan.ncmds),
	      t.nextra ? "from Linux for this mode" : "kept");
	if (dp)
		FBLOG("modes: DisplayPort, link untouched: DTO %u + %u / %u -> %u + %u / %u, DP_VID_M 0x%x -> 0x%x",
		      dtoNow.integer, dtoNow.phase, dtoNow.modulo, t.dto.integer, t.dto.phase, t.dto.modulo,
		      vidMNow, t.vidM);
	if (runPlan(modePlan)) {
		liveTiming = m.t;
		if (t.nextra)
			reqTiming = m.t;
		// The firmware taking SET_PIXEL_CLOCK does not show the PLL runs at
		// the new rate: time the OTG's frames. An unchanged clock would give
		// the old pixel clock over the new totals, not this refresh.
		const uint64_t periodNs = measureFramePeriodNs();
		const uint32_t mHz = periodNs ? static_cast<uint32_t>(1000000000000ULL / periodNs) : 0;
		FBLOG("modes: now %ux%u@%u.%03u, measured %u.%03u Hz", m.t.hActive, m.t.vActive,
		      m.refreshMilliHz / 1000, m.refreshMilliHz % 1000, mHz / 1000, mHz % 1000);
		// Where the picture sits: the scaler's mode and the rectangles around it,
		// and the plane's underflow flags (a picture cut off at the top, 2026-10-06).
		FBLOG("modes: scaler mode 0x%08x autocal 0x%08x%s; viewport 0x%08x at 0x%08x, recout 0x%08x at 0x%08x, mpc "
		      "0x%08x; plane blank end 0x%08x; HUBP%u_DCHUBP_CNTL 0x%08x", regReadDmu(2, 0x0d08 + dppOff()),
		      regReadDmu(2, 0x0d19 + dppOff()), t.scalerOff ? " (turned off: stretched console)" : "",
		      regReadDmu(2, 0x05eb + hubpOff()), regReadDmu(2, 0x05e9 + hubpOff()), regReadDmu(2, 0x0d1f + dppOff()),
		      regReadDmu(2, 0x0d1e + dppOff()), regReadDmu(2, 0x0d20 + dppOff()), regReadDmu(2, 0x063b + hubpOff()),
		      pipe.hubp, regReadDmu(2, 0x05f4 + hubpOff()));
		return kIOReturnSuccess;
	}

	// Put the previous timing back so the display is not left dark.
	aim(m.t, liveTiming);
	bool restored = ModeSet::build(t, modePlan, &why) && runPlan(modePlan);
	FBLOG("modes: switch to id %u failed, previous mode %s", m.id,
	      restored ? "restored" : "NOT restored");
	return kIOReturnIOError;
}


bool RDNA4Device::bootUhdPipe(uint8_t pipeIndex) const
{
	Edid::DetailedTiming uhd {};

	if (!pipe.valid() || !pipe.isTmds() || !edidLen || !Edid::vicTiming(97, uhd) || !Edid::hdmi2Caps(edidData, edidLen).scdc) {
		FBLOG("boot4k: pipe %u: no HDMI boot display that announces SCDC (TMDS %d, EDID %zu bytes)", pipeIndex, pipe.isTmds(),
		      static_cast<size_t>(edidLen));
		return false;
	}

	// What the adapter carries: in RGB, or in 4:2:0 at half the clock if the sink takes 3840x2160 so.
	const uint32_t adaptorKHz = dpDualMaxKHz(pipe.hpd);
	const bool y420 = adaptorKHz && adaptorKHz < uhd.pixelClockKHz;

	if (y420 ? adaptorKHz < ModeSet::tmdsKHz(uhd.pixelClockKHz, true) || !Edid::takesY420(edidData, edidLen, 97)
	         : !Edid::hasVic(edidData, edidLen, 97)) {
		FBLOG("boot4k: pipe %u: the sink does not take 3840x2160 through the adapter (%u kHz, %s)", pipeIndex, adaptorKHz,
		      y420 ? "4:2:0" : "RGB");
		return false;
	}

	for (size_t i = 0; i < Pipe2::configCount(); i++) {
		const Pipe2::Config &c = Pipe2::configAt(i);

		if (c.hpd == pipe.hpd && c.pipe == pipeIndex && !c.dp && Pipe2::isUhd(c) && c.y420 == y420)
			return true;
	}

	FBLOG("boot4k: pipe %u: no 3840x2160 HDMI table (%s) for HPD%u on it (DP++ %d)", pipeIndex, y420 ? "4:2:0" : "RGB", pipe.hpd,
	      dpDual(pipe.hpd));
	return false;
}

bool RDNA4Device::releaseBootPipe()
{
	AtomBios::DisplayPath path {};
	uint8_t ddcLine = 0;

	if (bootReleased)
		return true;

	if (!pipe.valid() || !pipe.isTmds() || pipe.dig >= Pipe::kMaxDig || !pathForHpd(pipe.hpd, path, &ddcLine)) {
		FBLOG("boot4k: the boot pipe is not released: no HDMI stream of the firmware's to take down");
		return false;
	}

	constexpr uint32_t kDigFifoCtrl0 = 0x209b;   // DIGn_DIG_FIFO_CTRL0
	const uint32_t dig = pipe.dig * Pipe::Reg::kDigStride;
	Dmub::Cmd cmd;

	DpPhy::buildDisable(cmd, pipe.link, pipe.hpd, static_cast<uint8_t>(path.connectorObjId & 0xff));

	// The stream alone goes down: the plane and its timing generator run on and keep their DET segments. With them
	// stopped and their segments given to the head's plane, both 4K planes underflowed for good (HUBP_SEG_ALLOC_ERR on the
	// new one: the monitors showed green, card, 2026-11-06).
	const ModeSet::Step steps[] = {
		{ ModeSet::Op::Update, 2, false, kDigFifoCtrl0 + dig, 0x1, 0x0, 0, "DIG_FIFO_ENABLE off" },
		{ ModeSet::Op::Update, 2, false, Pipe::Reg::kDigFeEnCntl + dig, 0x1, 0x0, 0, "DIG_FE_ENABLE: the stream encoder off" },
		{ ModeSet::Op::Dmub, 0, false, 0, 0, 0, 0, "DIG1_TRANSMITTER_CONTROL disable" },
		{ ModeSet::Op::Update, 2, false, Pipe::Reg::kDigBeEnCntl + dig, 0x1, 0x0, 0, "DIG_BE_ENABLE: the link encoder off" },
	};

	FBLOG("boot4k: taking the firmware's stream down: DIG%u link %u HPD%u (OTG%u and HUBP%u run on)", pipe.dig, pipe.link,
	      pipe.hpd, pipe.otg, pipe.hubp);

	if (!runSteps(steps, sizeof(steps) / sizeof(steps[0]), &cmd, 1, "boot4k", otgOff())) {
		FBLOG("boot4k: the firmware's stream did not go down cleanly");
		return false;
	}

	FBLOG("boot4k: released; DIG%u_DIG_FE_EN_CNTL=0x%08x DIG_BE_EN_CNTL=0x%08x OTG%u_OTG_CONTROL=0x%08x", pipe.dig,
	      regReadDmu(2, Pipe::Reg::kDigFeEnCntl + dig), regReadDmu(2, Pipe::Reg::kDigBeEnCntl + dig), pipe.otg,
	      regReadDmu(2, Pipe::Reg::kOtgControl + otgOff()));

	// Its connector is free for a head: no DIG, link or pin of the boot pipe's any more (its OTG and HUBP stay
	// known: a head's plane copies the HUBP's VM aperture)
	pipe.dig = Pipe::kNone;
	pipe.link = Pipe::kNone;
	pipe.hpd = 0;
	bootReleased = true;

	return true;
}

// ---------------------------------------------------------------------------
// The second pipe
// ---------------------------------------------------------------------------

// What each step of a plan would do, with the register as it is now, as the
// registry property AMDRadeonRDNA4,Pipe2 (lines joined with " ## ", like
// AMDRadeonRDNA4,Cursor): several hundred lines do not survive the kernel log.
void RDNA4Device::surveySteps(const ModeSet::Step *steps, size_t count, const Dmub::Cmd *cmds) {
	constexpr size_t kCap = 96 * 1024;
	char *text = static_cast<char *>(IOMalloc(kCap));
	if (!text)
		return;
	size_t len = 0, differ = 0, unmet = 0;
	text[0] = '\0';
	for (size_t i = 0; i < count && len + 256 < kCap; i++) {
		const ModeSet::Step &s = steps[i];
		const uint32_t cur = regReadDmu(s.seg, s.dword);
		char line[224];
		switch (s.op) {
		case ModeSet::Op::Write:
		case ModeSet::Op::Update:
		case ModeSet::Op::Copy: {
			const uint32_t next = s.op == ModeSet::Op::Write ? s.value
			                    : s.op == ModeSet::Op::Copy ? regReadDmu(s.seg, s.arg)
			                    : (cur & ~s.mask) | s.value;
			differ += next != cur;
			snprintf(line, sizeof(line), "%lu %u:%04x %08x %s %08x %s", static_cast<unsigned long>(i),
			         s.seg, s.dword, cur, next == cur ? "==" : "->", next, s.what);
			break;
		}
		case ModeSet::Op::Require:
		case ModeSet::Op::WaitValue: {
			const bool met = cur != 0xFFFFFFFF && (cur & s.mask) == s.value;
			unmet += s.op == ModeSet::Op::Require && !met;
			snprintf(line, sizeof(line), "%lu %u:%04x %08x %s &%08x=%08x %s%s", static_cast<unsigned long>(i),
			         s.seg, s.dword, cur, s.op == ModeSet::Op::Require ? "require" : "wait", s.mask, s.value,
			         s.what, met ? "" : " (not so now)");
			if (s.op == ModeSet::Op::Require)
				FBLOG("pipe2: %s", line);
			break;
		}
		case ModeSet::Op::Dmub:
			snprintf(line, sizeof(line), "%lu dmub %08x %08x %08x %08x %s", static_cast<unsigned long>(i),
			         cmds[s.arg][0], cmds[s.arg][1], cmds[s.arg][2], cmds[s.arg][3], s.what);
			break;
		case ModeSet::Op::Scdc:
			snprintf(line, sizeof(line), "%lu scdc DDC%u TMDS_CONFIG=%u %s", static_cast<unsigned long>(i),
			         s.arg, s.value, s.what);
			break;
		case ModeSet::Op::Aux:
		case ModeSet::Op::Train:
			snprintf(line, sizeof(line), "%lu %s %s", static_cast<unsigned long>(i),
			         s.op == ModeSet::Op::Train ? "train the link:" : "aux write:", s.what);
			break;
		default:
			snprintf(line, sizeof(line), "%lu delay %u us %s", static_cast<unsigned long>(i), s.arg, s.what);
		}
		len += snprintf(text + len, kCap - len, "%s ## ", line);
	}
	if (owner)
		owner->setProperty("AMDRadeonRDNA4,Pipe2", text);
	FBLOG("pipe2: survey of %lu steps in the registry (AMDRadeonRDNA4,Pipe2): %lu would change a register, "
	      "%lu requirement(s) not met; nothing written", static_cast<unsigned long>(count),
	      static_cast<unsigned long>(differ), static_cast<unsigned long>(unmet));
	IOFree(text, kCap);
}

bool RDNA4Device::lightSecondPipe(uint32_t level, IOPhysicalAddress64 surfacePhys) {
	if (!pipe.valid() || !rmmio || !ipDiscovery.isValid()) {
		FBLOG("pipe2: not lit: no lit pipe to stand next to");
		return false;
	}
	// The plan for the connector the second display is on.
	if (!usePlanFor(xp().hpd)) {
		FBLOG("pipe2: not lit: the second display is on HPD%u, a connector there is no plan for (a DisplayPort "
		      "one is off with rdna4-head2dp=0)", xp().hpd);
		return false;
	}
	const Pipe2::Config &c = Pipe2::config();
	if (pipe.otg == c.pipe || pipe.opp == c.pipe || pipe.hubp == c.pipe || pipe.dig == c.dig ||
	    pipe.link == c.link) {
		FBLOG("pipe2: not lit: the firmware's pipe (OTG%u OPP%u HUBP%u DIG%u link %u) uses a block of the "
		      "plan's (pipe %u, DIG%u, link %u)", pipe.otg, pipe.opp, pipe.hubp, pipe.dig, pipe.link,
		      c.pipe, c.dig, c.link);
		return false;
	}
	AtomBios::DisplayPath path {};
	uint8_t ddcLine = 0;
	if (!pathForHpd(c.hpd, path, &ddcLine) || Pipe2::linkOfEncoder(path.encoderObjId) != c.link) {
		FBLOG("pipe2: not lit: the plan is for a connector on HPD%u and link %u, which this board's VBIOS "
		      "does not have", c.hpd, c.link);
		return false;
	}

	if (c.dp && ((!xp().sink.maxRate && !readSecondDpSink(ddcLine)) || !pickSecondLink(c.timing.pixelClockKHz))) {
		FBLOG("pipe2: not lit: no DisplayPort sink with a link for the mode on AUX%u", ddcLine);
		return false;
	}

	Pipe2::Target t {};
	t.litHubp = pipe.hubp;
	t.encoderObjId = path.encoderObjId;
	t.connectorObjId = static_cast<uint8_t>(path.connectorObjId & 0xff);
	t.linkRate = xp().link.rate;
	// Only level 3 stops at the stream; the survey (2) covers the whole plan.
	t.depth = level == 3 ? Pipe2::Depth::Stream : Pipe2::Depth::Plane;
	t.sinkScdc = Edid::hdmi2Caps(xp().edid, xp().edidLen).scdc;
	t.ddcLine = ddcLine;
	// The lit pipe scans the console from the address in its HUBP; the second
	// surface lies in the same memory, so the same distance further on.
	constexpr uint32_t kSurfaceLo = 0x060a, kSurfaceHi = 0x060b;   // HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS(_HIGH)
	const uint64_t lit = regReadDmu(2, kSurfaceLo + hubpOff()) |
	                     (static_cast<uint64_t>(regReadDmu(2, kSurfaceHi + hubpOff()) & 0xffff) << 32);
	if (surfacePhys <= fbPhysBase || lit == 0) {
		FBLOG("pipe2: not lit: no scanout address (console 0x%llx at 0x%llx, surface 0x%llx)", fbPhysBase,
		      lit, surfacePhys);
		return false;
	}
	t.surface = lit + (surfacePhys - fbPhysBase);

	// The plane needs its share of the DET buffer; what the lit pipes hold is
	// theirs (taking it back means reprogramming them), an idle pipe's is
	// given back first.
	freeIdleDet(c);
	uint32_t det[4] {}, others = 0;
	for (uint32_t i = 0; i < 4; i++) {
		det[i] = regReadDmu(c.detSeg, c.detCtrl[i]) & 0x1f;
		if (i != c.pipe)
			others += det[i];
	}
	others += shrinkCompbuf(c, others);
	t.detSegments = static_cast<uint8_t>(detShare(c.detSegments, others));
	const bool detFits = t.detSegments != 0;
	FBLOG("pipe2: DET segments %u %u %u %u of %u: %u for the plane (plan %u) %s", det[0], det[1], det[2], det[3],
	      Pipe2::kDetSegmentsTotal, t.detSegments, c.detSegments, detFits ? "fit" : "do NOT fit");

	auto *plan = static_cast<Pipe2::Plan *>(IOMalloc(sizeof(Pipe2::Plan)));
	if (!plan)
		return false;
	const char *why = "";
	bool ok = Pipe2::build(t, *plan, &why);
	if (!ok) {
		FBLOG("pipe2: not lit: %s", why);
	} else if (level <= 2) {
		FBLOG("pipe2: pipe %u DIG%u link %u HPD%u, %ux%u at %u kHz, surface at 0x%llx: %lu steps, "
		      "%lu DMUB commands", c.pipe, c.dig, c.link, c.hpd, c.timing.hActive, c.timing.vActive,
		      c.timing.pixelClockKHz, t.surface, static_cast<unsigned long>(plan->count),
		      static_cast<unsigned long>(plan->ncmds));
		surveySteps(plan->steps, plan->count, plan->cmds);
	} else if (t.depth == Pipe2::Depth::Plane && !detFits) {
		FBLOG("pipe2: not lit: the lit pipe holds the DET buffer; rdna4-head2=3 lights the stream alone");
		ok = false;
	} else {
		FBLOG("pipe2: lighting pipe %u DIG%u link %u HPD%u, %ux%u at %u kHz, %s: %lu steps, %lu DMUB "
		      "commands", c.pipe, c.dig, c.link, c.hpd, c.timing.hActive, c.timing.vActive,
		      c.timing.pixelClockKHz, t.depth == Pipe2::Depth::Plane ? "stream and plane" : "stream only",
		      static_cast<unsigned long>(plan->count), static_cast<unsigned long>(plan->ncmds));
		// Before the plan runs: its training step reads the connector from here.
		xp().target = t;
		xp().target.now = c.timing;
		xp().connectorObjId = path.connectorObjId;
		ok = runSteps(plan->steps, plan->count, plan->cmds, plan->ncmds, "pipe2", c.pipe * Pipe::Reg::kOtgStride,
		              plan->aux);
		xp().lit = xp().on = ok;
		xp().target.rgb = false;
		// Whether it runs: the frame rate of its timing generator, and the
		// underflow flags of the plane and the OPTC (HUBPn_DCHUBP_CNTL,
		// ODMn_OPTC_INPUT_GLOBAL_CONTROL), raw.
		const uint64_t periodNs = measureFramePeriodNs(c.pipe * Pipe::Reg::kOtgStride);
		const uint32_t mHz = periodNs ? static_cast<uint32_t>(1000000000000ULL / periodNs) : 0;
		const uint32_t want = c.timing.refreshMilliHz();
		FBLOG("pipe2: %s; OTG%u measured %u.%03u Hz (the mode is %u.%03u); HUBP%u_DCHUBP_CNTL=0x%08x "
		      "ODM%u_OPTC_INPUT_GLOBAL_CONTROL=0x%08x", ok ? "plan ran" : "plan STOPPED", c.pipe,
		      mHz / 1000, mHz % 1000, want / 1000, want % 1000, c.pipe,
		      regReadDmu(2, 0x05f4 + c.pipe * Pipe::Reg::kHubpStride), c.pipe,
		      regReadDmu(2, 0x1aca + c.pipe * Pipe::Reg::kOdmStride));
		if (ok && level >= 4)
			comparePipeColour(*plan);
		if (ok) {
			logUnderflow("pipe2: after lighting");
			snapAfterLight(*plan);
		}
		// With the stream lit, what the plane would still change.
		t.depth = Pipe2::Depth::Plane;
		if (ok && level == 3 && Pipe2::build(t, *plan, &why))
			surveySteps(plan->steps, plan->count, plan->cmds);
	}
	IOFree(plan, sizeof(Pipe2::Plan));
	return ok;
}

/*
 * The planes' underflow flags (HUBPn_DCHUBP_CNTL.HUBP_UNDERFLOW_STATUS,
 * bits 30:28), sticky: what lighting itself caused, then, cleared
 * (HUBP_UNDERFLOW_CLEAR, bit 31), what half a second of scanout adds. A
 * plane whose DET share or clocks are too small for its mode keeps
 * underflowing.
 */
void RDNA4Device::logUnderflow(const char *when)
{
	constexpr uint32_t kDchubpCntl = 0x05f4;
	constexpr uint32_t kClear = 1u << 31;
	uint32_t before[4], after[4];

	IOSleep(200);

	for (uint32_t i = 0; i < 4; i++) {
		const uint32_t reg = kDchubpCntl + i * Pipe::Reg::kHubpStride;
		const uint32_t v = regReadDmu(2, reg);

		before[i] = v >> 28 & 0x7;
		regWriteDmu(2, reg, v | kClear);
		regWriteDmu(2, reg, v & ~kClear);
	}

	IOSleep(500);

	for (uint32_t i = 0; i < 4; i++)
		after[i] = regReadDmu(2, kDchubpCntl + i * Pipe::Reg::kHubpStride) >> 28 & 0x7;

	FBLOG("%s: underflow HUBP0..3 %u %u %u %u, then in 0.5 s %u %u %u %u", when, before[0], before[1],
	      before[2], before[3], after[0], after[1], after[2], after[3]);
}

// One pipe was programmed by the firmware and one by Linux's code: where the
// two colour paths differ is where a difference in the picture can come from.
// Geometry and instance numbers differ by nature (DSCL sizes, TOP_SEL, DPG
// dimensions).
void RDNA4Device::comparePipeColour(const Pipe2::Plan &plan) {
	const Pipe2::Config &c = Pipe2::config();
	uint32_t compared = 0, differ = 0;
	auto planStep = [&](uint8_t seg, uint32_t dword, size_t before) -> const ModeSet::Step * {
		for (size_t i = 0; i < before; i++) {
			const ModeSet::Step &s = plan.steps[i];
			if ((s.op == ModeSet::Op::Write || s.op == ModeSet::Op::Update) && s.seg == seg && s.dword == dword)
				return &s;
		}
		return nullptr;
	};
	auto compare = [&](uint8_t seg, uint32_t dword, const char *what) {
		const int twin = Pipe2::colourTwin(seg, dword, c.pipe, pipe.hubp, pipe.opp);
		if (twin < 0)
			return;
		compared++;
		const uint32_t mine = regReadDmu(seg, dword), lit = regReadDmu(seg, static_cast<uint32_t>(twin));
		if (mine == lit)
			return;
		differ++;
		FBLOG("pipe2: colour: %u:%04x %08x, lit pipe %u:%04x %08x  %s", seg, dword, mine, seg, twin, lit, what);
	};
	// What the plan set on its pipe, each register once ...
	for (size_t i = 0; i < plan.count; i++) {
		const ModeSet::Step &s = plan.steps[i];
		if ((s.op == ModeSet::Op::Write || s.op == ModeSet::Op::Update) && !planStep(s.seg, s.dword, i))
			compare(s.seg, s.dword, s.what);
	}
	// ... and what it left at reset there, in the blocks that can be read whole.
	for (const Pipe2::ColourBlock &b : Pipe2::kColourBlocks) {
		const uint32_t first = b.first + c.pipe * b.stride;
		for (uint32_t dword = first; b.whole && dword < first + b.stride; dword++)
			if (!planStep(b.seg, dword, plan.count))
				compare(b.seg, dword, "(not set by the plan)");
	}
	FBLOG("pipe2: colour: %u registers of pipe %u compared with the lit pipe's (HUBP%u, OPP%u): %u differ",
	      compared, c.pipe, pipe.hubp, pipe.opp, differ);
}

// What a DisplayPort plan needs of its sink before it can train: the
// receiver capability, read where the sink's EDID is.
bool RDNA4Device::readSecondDpSink(uint8_t aux) {
	uint8_t caps[16] {};
	xp().aux = aux;
	xp().sink = {};
	if (!dpcdRead(aux, 0x000, caps, sizeof(caps)) || !DpTrain::parseCaps(caps, sizeof(caps), xp().sink)) {
		xp().sink = {};
		FBLOG("pipe2: no DisplayPort receiver capability on AUX%u", aux);
		return false;
	}
	FBLOG("pipe2: DisplayPort sink on AUX%u: DPCD %u.%u, up to %u lane(s) at rate 0x%02x%s%s", aux, caps[0] >> 4,
	      caps[0] & 0xf, xp().sink.maxLanes, xp().sink.maxRate, xp().sink.tps3 ? ", TPS3" : "",
	      xp().sink.tps4 ? ", TPS4" : "");
	return true;
}

// The smallest link of the sink's that carries a mode at 8 bpc: what the next
// training step brings up.
bool RDNA4Device::pickSecondLink(uint32_t pixelClockKHz) {
	if (!xp().sink.maxRate || !DpTrain::pickLink(xp().sink, pixelClockKHz, 24, xp().link)) {
		FBLOG("pipe2: the DisplayPort sink has no link for %u kHz", pixelClockKHz);
		return false;
	}
	FBLOG("pipe2: %u lane(s) at rate 0x%02x for %u kHz", xp().link.lanes, xp().link.rate, pixelClockKHz);
	return true;
}

bool RDNA4Device::readDpDto(uint8_t otg, ModeSet::DpDto &out, uint32_t *cntlOut) {
	constexpr uint32_t kOtgPixelRateCntl = 0x0080, kDpDtoPhase = 0x0081, kDpDtoModulo = 0x0082;   // + 4 per OTG, base_idx 1
	constexpr uint32_t kOtgPixelRateDiv = 0x006f;
	const uint32_t cntl = regReadDmu(1, kOtgPixelRateCntl + 4 * otg);
	if (cntlOut)
		*cntlOut = cntl;
	// DP_DTO_ENABLE and PIPE_DTO_SRC_SEL, as dccg401_set_dp_dto leaves them
	if (cntl == 0xFFFFFFFF || (cntl & 0x1010) != 0x1010)
		return false;
	out.phase = regReadDmu(1, kDpDtoPhase + 4 * otg);
	out.modulo = regReadDmu(1, kDpDtoModulo + 4 * otg);
	out.integer = (regReadDmu(1, kOtgPixelRateDiv) >> (1 + 5 * otg)) & 0xf;
	return true;
}

// The Op::Train step of a DisplayPort plan.
bool RDNA4Device::trainSecondLink() {
	const Pipe2::Config &c = Pipe2::config();
	DpTrain::Report rep {};
	const DpTrain::Result r = trainLink(xp().aux, c.link, c.hpd, xp().target.connectorObjId, xp().sink, xp().link,
	                                    false, rep);
	char tries[160];
	FBLOG("pipe2: link %u, %u lane(s) at rate 0x%02x: %s after %u attempt(s) (%s): swing %u, pre-emphasis %u, lanes "
	      "%02x %02x, aligned %u", c.link, xp().link.lanes, xp().link.rate, DpTrain::resultName(r), rep.attempts,
	      triesText(rep, tries, sizeof(tries)), rep.swing, rep.preEmphasis, rep.status[0], rep.status[1],
	      rep.status[2] & 1);
	return r == DpTrain::Result::Ok;
}

size_t RDNA4Device::extraHeads() {
	// Every connector beside the boot one's unless asked for fewer: three
	// displays ran on the card, a head without a monitor waits offline.
	static uint32_t heads = 0;
	if (!heads) {
		uint32_t want = kMaxExtra;
		Trial::option("rdna4-heads", want);
		heads = want < 1 ? 1 : want > kMaxExtra ? kMaxExtra : want;
		FBLOG("pipe2: up to %u display(s) beside the boot one (rdna4-heads)", heads);
	}
	return heads;
}

size_t RDNA4Device::freeExtraSlot() const {
	size_t slot = 0;
	while (slot < kMaxExtra && (extra[slot].edidLen || extra[slot].lit))
		slot++;
	return slot;
}

bool RDNA4Device::hpdTakenByOther(uint8_t hpd) const {
	for (size_t i = 0; i < kMaxExtra; i++)
		if (i != extraSlot && extra[i].hpd == hpd && (extra[i].edidLen || extra[i].lit))
			return true;
	return false;
}

void RDNA4Device::selectExtra(size_t slot) {
	if (slot >= kMaxExtra || slot == extraSlot)
		return;
	extraSlot = slot;
	useSlotTable();
}

void RDNA4Device::useSlotTable() {
	// Its connector's table, or until it has a sink its pipe's first.
	const ExtraPipe &x = xp();
	if (!x.hpd || !Pipe2::use(x.hpd, x.lit ? x.hdmi : dpDual(x.hpd), x.pipe, x.uhd, x.y420))
		Pipe2::use(3, false, x.pipe);
}

/* The 3840x2160 table of the connector on that pin for that stream, or nullptr. */
static const Pipe2::Config *uhdConfigFor(uint8_t hpd, bool dp, bool y420 = false)
{
	for (size_t i = 0; i < Pipe2::configCount(); i++) {
		const Pipe2::Config &c = Pipe2::configAt(i);

		if (c.hpd == hpd && c.dp == dp && c.y420 == y420 && Pipe2::isUhd(c))
			return &c;
	}

	return nullptr;
}

/*
 * Whether a DisplayPort sink takes the plan's timing: its preferred one (a 4K
 * monitor's CVT-RB 3840x2160@60), on a link it can train.
 */
bool RDNA4Device::dpSinkTakes(const Edid::DetailedTiming &t) const
{
	Edid::DetailedTiming pref {};
	DpTrain::Link link {};

	if (!Edid::preferredTiming(xp().edid, xp().edidLen, pref) || !Edid::sameTiming(pref, t))
		return false;

	return xp().sink.maxRate && DpTrain::pickLink(xp().sink, t.pixelClockKHz, 24, link);
}

/*
 * Whether the pipe's sink, on that pin, can be lit at 3840x2160; `y420` says
 * whether in YCbCr 4:2:0. That is for a DP++ adapter that carries no more than
 * 300 MHz: the TMDS clock is then half the pixel clock, as amdgpu runs it
 * (a Dell at 3840x2160@60 through the test rig's adapters, 2026-11-04).
 */
bool RDNA4Device::sinkTakesUhd(uint8_t hpd, bool *y420) const
{
	const bool dp = Pipe2::hasDpTable(hpd) && !dpDual(hpd);
	const Pipe2::Config *c = uhdConfigFor(hpd, dp);
	const Pipe2::Config *half = dpDual(hpd) ? uhdConfigFor(hpd, false, true) : nullptr;
	const uint32_t adaptorKHz = dpDual(hpd) ? dpDualMaxKHz(hpd) : 0;
	bool dummy;
	bool &y = y420 ? *y420 : dummy;

	y = false;

	if (!c)
		return false;

	if (dp)
		return dpSinkTakes(c->timing);

	if (!Edid::hdmi2Caps(xp().edid, xp().edidLen).scdc)
		return false;

	const uint8_t vic = Edid::vicOf(c->timing);

	if (!adaptorKHz || adaptorKHz >= c->timing.pixelClockKHz)
		return Edid::hasVic(xp().edid, xp().edidLen, vic);

	// Behind a DP++ adapter that does not carry the table's clock: in 4:2:0
	// at half of it, if the sink takes the format so.
	y = half && adaptorKHz >= ModeSet::tmdsKHz(half->timing.pixelClockKHz, true) &&
	    Edid::takesY420(xp().edid, xp().edidLen, Edid::vicOf(half->timing));

	return y;
}

/* Whether a pipe scans out: the boot pipe, or an extra one that is lit. */
bool RDNA4Device::pipeInUse(uint32_t p) const
{
	if (p == pipe.otg)   // released too, its timing generator runs on and its plane keeps its DET (releaseBootPipe)
		return true;

	for (const ExtraPipe &x : extra) {
		if (x.pipe == p && x.lit)
			return true;
	}

	return false;
}

/* Give the DET segments of pipes nothing scans out from back to the pool. */
void RDNA4Device::freeIdleDet(const Pipe2::Config &c)
{
	for (uint32_t i = 0; i < 4; i++) {
		const uint32_t v = regReadDmu(c.detSeg, c.detCtrl[i]);

		if (i == c.pipe || pipeInUse(i) || !(v & 0x1f))
			continue;

		regWriteDmu(c.detSeg, c.detCtrl[i], v & ~0x1fu);
		FBLOG("pipe2: DET of idle pipe %u (%u segments) given back", i, v & 0x1f);
	}
}

/*
 * The DET buffers and the compressed buffer share the 21 segments
 * (dcn401_program_compbuf_segments). The firmware leaves the compressed
 * buffer some; when the plan's DET would not fit next to the other pipes'
 * and it, it shrinks (shrinking is always allowed): the scanout surfaces
 * here are linear, nothing uses it. With it overcommitted every plane
 * underflowed (two 4K pipes, card, 2026-11-04). Returns what it keeps.
 */
uint32_t RDNA4Device::shrinkCompbuf(const Pipe2::Config &c, uint32_t others)
{
	constexpr uint32_t kCompbufCtrl = 0x04ba;
	const uint32_t v = regReadDmu(c.detSeg, kCompbufCtrl);
	const uint32_t now = v >> 8 & 0x1f;
	const uint32_t room = others + c.detSegments < Pipe2::kDetSegmentsTotal ?
			      Pipe2::kDetSegmentsTotal - others - c.detSegments : 0;
	uint32_t got = now;

	if (now <= room)
		return now;

	regWriteDmu(c.detSeg, kCompbufCtrl, (v & ~0x1fu) | room);

	for (int i = 0; i < 100 && got != room; i++) {
		IOSleep(1);
		got = regReadDmu(c.detSeg, kCompbufCtrl) >> 8 & 0x1f;
	}

	FBLOG("pipe2: compressed buffer %u -> %u segments%s", now, room,
	      got == room ? "" : " (not taken yet)");

	return got;
}

/*
 * The DET segments a plane gets next to `others`: the plan's if they fit,
 * else what is left down to kMinDetSegments, else 0. DML gave the plan's
 * for the pipe alone; with more displays it would give each less too.
 */
uint32_t RDNA4Device::detShare(uint32_t plan, uint32_t others) const
{
	const uint32_t left = others < Pipe2::kDetSegmentsTotal ? Pipe2::kDetSegmentsTotal - others : 0;

	if (plan <= left)
		return plan;

	return left >= kMinDetSegments ? left : 0;
}

bool RDNA4Device::usePlanFor(uint8_t hpd) {
	const Pipe2::Config was = Pipe2::config();
	// A lit pipe keeps the size and the kind of stream it is lit with.
	bool y420 = xp().y420;
	const bool uhd = xp().lit ? xp().uhd : uhdClocks && sinkTakesUhd(hpd, &y420);
	const bool hdmi = xp().lit ? xp().hdmi : dpDual(hpd);
	if (!Pipe2::use(hpd, hdmi, xp().pipe, uhd, y420))
		return false;
	if (Pipe2::config().dp && !pipe2DpAllowed) {
		Pipe2::use(was.hpd, !was.dp, xp().pipe, Pipe2::isUhd(was), was.y420);
		return false;
	}
	xp().uhd = uhd;
	xp().y420 = Pipe2::config().y420;
	xp().hdmi = !Pipe2::config().dp;
	return true;
}

/*
 * A lit pipe whose DisplayPort connector now has the other kind of sink: a
 * DisplayPort monitor in place of an HDMI one behind a DP++ adapter, or the
 * other way round. The pipe takes the connector's table for that kind, at
 * the size the new sink takes, and its stream moves there at the next
 * secondPipeTo, as between connectors. Until then a wake by the old table
 * would drive the new sink with the wrong kind of stream.
 */
bool RDNA4Device::adoptSinkKind(uint8_t hpd)
{
	const Pipe2::Config was = Pipe2::config();
	const bool hdmi = dpDual(hpd);
	bool y420 = false;
	const bool uhd = uhdClocks && sinkTakesUhd(hpd, &y420);

	if (!xp().lit || hpd != was.hpd || !Pipe2::hasDpTable(hpd) || hdmi == xp().hdmi)
		return false;

	if (xp().on) {
		runSecondPipePart(Pipe2::Part::Sleep);
		xp().on = false;
	}

	if (!Pipe2::use(hpd, hdmi, xp().pipe, uhd, y420) || (Pipe2::config().dp && !pipe2DpAllowed)) {
		Pipe2::use(was.hpd, !was.dp, xp().pipe, Pipe2::isUhd(was), was.y420);
		FBLOG("pipe2: HPD%u has another kind of sink, and no table for it: the pipe stays off", hpd);
		return false;
	}

	xp().hdmi = hdmi;
	xp().uhd = uhd;
	xp().y420 = Pipe2::config().y420;
	xp().moveOwed = true;
	xp().moveToPlan = true;
	FBLOG("pipe2: HPD%u now has %s: the pipe moves to its %ux%u table", hpd,
	      hdmi ? "an HDMI sink behind a DP++ adapter" : "a DisplayPort sink",
	      Pipe2::config().timing.hActive, Pipe2::config().timing.vActive);

	return true;
}

void RDNA4Device::secondLinkBack() {
	if (!xp().lit || !xp().on || !Pipe2::config().dp)
		return;
	uint8_t status[3] {};
	if (!dpcdRead(xp().aux, 0x202, status, sizeof(status))) {
		FBLOG("pipe2: the display's pin is back but its DPCD does not answer on AUX%u: link left alone", xp().aux);
	} else if (DpTrain::linkUp(status, xp().link.lanes)) {
		FBLOG("pipe2: the display's pin is back and its link is up (lanes %02x %02x, aligned %u)", status[0],
		      status[1], status[2] & 1);
	} else {
		FBLOG("pipe2: the display's pin is back and its link is down (lanes %02x %02x, aligned %u): training it "
		      "again", status[0], status[1], status[2] & 1);
		runSecondPipePart(Pipe2::Part::Sleep);
		xp().on = pickSecondLink(xp().target.now.pixelClockKHz) && runSecondPipePart(Pipe2::Part::Wake);
	}
}

uint8_t RDNA4Device::secondSinkHpd(bool orAnother) {
	// DC_GPIO_HPD_Y: one byte per pin, HPD1 lowest (probeEDID).
	const uint32_t hpdY = regReadDmu(2, 0x28f7);
	if (hpdY == 0xFFFFFFFF)
		return 0;
	auto high = [&](uint8_t hpd) { return hpd && ((hpdY >> (8 * (hpd - 1))) & 1); };
	const Pipe2::Config &now = Pipe2::config();
	if (xp().lit && (high(now.hpd) || !orAnother))   // its own connector first
		return high(now.hpd) ? now.hpd : 0;
	for (size_t i = 0; i < Pipe2::configCount(); i++) {
		const Pipe2::Config &c = Pipe2::configAt(i);
		if (c.pipe != xp().pipe || c.hpd == pipe.hpd || hpdTakenByOther(c.hpd) || !high(c.hpd) ||
		    (c.dp && !pipe2DpAllowed))
			continue;

		// A DisplayPort connector has a DisplayPort and an HDMI table. The
		// HDMI one stands for it when its sink is behind a DP++ adapter,
		// and while the second pipe may not drive DisplayPort: a sink not
		// read yet is offered, and readSecondEdid finds its bus.
		if (Pipe2::hasDpTable(c.hpd)) {
			const bool hdmi = dpDual(c.hpd) || !pipe2DpAllowed;
			if (c.dp == hdmi)
				continue;
		}

		// A lit pipe moves only between connectors with an HDMI stream
		// (secondPipeTo).
		if (!xp().lit || (!c.dp && !now.dp))
			return c.hpd;
	}
	return 0;
}

bool RDNA4Device::secondPipeTo(uint8_t hpd) {
	if (!xp().lit)
		return false;
	const Pipe2::Config was = Pipe2::config();
	if (hpd != was.hpd) {
		AtomBios::DisplayPath path {};
		uint8_t ddcLine = 0;
		bool ok = !was.dp && usePlanFor(hpd);
		const Pipe2::Config &to = Pipe2::config();
		ok = ok && !to.dp && pipe.dig != to.dig && pipe.link != to.link && pathForHpd(hpd, path, &ddcLine) &&
		     Pipe2::linkOfEncoder(path.encoderObjId) == to.link;
		if (!ok) {
			FBLOG("pipe2: not moved from HPD%u to HPD%u: only between connectors with an HDMI "
			      "stream the boot display does not use", was.hpd, hpd);
			Pipe2::use(was.hpd, !was.dp, xp().pipe, Pipe2::isUhd(was), was.y420);
			return false;
		}
		FBLOG("pipe2: moving from HPD%u (DIG%u, link %u) to HPD%u (DIG%u, link %u) at %ux%u, %u kHz", was.hpd,
		      was.dig, was.link, to.hpd, to.dig, to.link, xp().target.now.hActive, xp().target.now.vActive,
		      xp().target.now.pixelClockKHz);
		if (xp().on) {   // the old link down first, by its own table
			Pipe2::use(was.hpd, !was.dp, xp().pipe, Pipe2::isUhd(was), was.y420);
			runSecondPipePart(Pipe2::Part::Sleep);
			Pipe2::use(hpd, !to.dp, xp().pipe, Pipe2::isUhd(to), to.y420);
			xp().on = false;
		}
		xp().target.encoderObjId = path.encoderObjId;
		xp().target.connectorObjId = static_cast<uint8_t>(path.connectorObjId & 0xff);
		xp().target.ddcLine = ddcLine;
		xp().connectorObjId = path.connectorObjId;
		xp().moveOwed = true;
	}
	if (!xp().moveOwed) {
		setSecondPipePower(true);
		return xp().on;
	}
	// The pipe belongs to this connector now, whatever happens next: a move
	// that stops part-way is done again from here at the next plug.
	// The mode set to the running timing: the pipe's clock from this
	// connector's PLL, its encoder and transmitter. Then this connector's sleep
	// and wake, which leave its encoder and link as lighting here would
	// (test/atomdump.cpp, testPipe2Move).
	const Pipe2::Config &c = Pipe2::config();
	const Edid::DetailedTiming at = xp().moveToPlan ? c.timing : xp().target.now;
	const Modes::Mode same { 0, at, at.refreshMilliHz(), false, Modes::SourceBoot };

	if (applySecondPipeMode(same) != kIOReturnSuccess || !runSecondPipePart(Pipe2::Part::Sleep) ||
	    !runSecondPipePart(Pipe2::Part::Wake)) {
		FBLOG("pipe2: the move to HPD%u did not complete: the display stays off until it is plugged in again", hpd);
		return false;
	}

	xp().moveOwed = false;
	xp().moveToPlan = false;
	xp().on = true;
	FBLOG("pipe2: now on HPD%u; DIG%u_DIG_FE_EN_CNTL=0x%08x SYMCLK%c_CLOCK_ENABLE=0x%08x", c.hpd, c.dig,
	      regReadDmu(2, Pipe::Reg::kDigFeEnCntl + c.dig * Pipe::Reg::kDigStride), 'A' + c.dig,
	      regReadDmu(1, 0x00a0 + c.dig));
	return true;
}

bool RDNA4Device::readSecondEdid(uint8_t hpd) {
	AtomBios::DisplayPath path {};
	uint8_t ddcLine = 0;
	uint8_t edid[256] {};
	static const uint8_t sig[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	if (!pathForHpd(hpd, path, &ddcLine))
		return false;
	// A DisplayPort sink's EDID comes over its AUX channel, and with it what
	// its receiver can do: it may be another monitor than before. Behind a
	// passive DP++ adapter the sink has no AUX channel: its EDID is on the
	// connector's DDC pads in I2C mode (probeEDID), and the second pipe
	// lights it with the connector's HDMI table.
	const AtomBios::ConnectorType type = AtomBios::connectorType(path.connectorObjId);
	const bool dpConnector = type == AtomBios::ConnectorDP ||
	                         type == AtomBios::ConnectorUSBC;
	bool viaAux = dpConnector && !dpDual(hpd);
	auto read = [&](uint8_t *to, uint8_t start) {
		return viaAux ? readEDID(ddcLine, to, 128, start)
		              : readEDIDI2C(ddcLine, to, 128, start);
	};

	bool got = read(edid, 0);

	// Another kind of sink than before on this connector: the other bus.
	if (!got && dpConnector) {
		viaAux = !viaAux;
		ddcPadMode(ddcLine, !viaAux);
		got = read(edid, 0);

		if (got)
			FBLOG("pipe2: HPD%u answers on %s: %s", hpd, viaAux ? "AUX" : "DDC",
			      viaAux ? "a DisplayPort sink" : "an HDMI sink behind a DP++ adapter");
	}

	// Silent on both buses: the pads back in the mode dpDual says, which
	// the next read starts from.
	if (dpConnector) {
		if (got)
			setDpDual(hpd, !viaAux);
		else
			ddcPadMode(ddcLine, dpDual(hpd));
	}

	if (got && dpDual(hpd))
		readDpDualAdaptor(hpd, ddcLine);

	if (!got || memcmp(edid, sig, sizeof(sig)) != 0 || (viaAux && !readSecondDpSink(ddcLine)))
		return false;
	// The extension says whether the sink has SCDC and carries more modes.
	const bool ext = edid[126] > 0 && read(edid + 128, 128);
	xp().edidLen = ext ? 256 : 128;
	xp().hpd = hpd;
	memcpy(xp().edid, edid, sizeof(edid));
	xp().target.sinkScdc = Edid::hdmi2Caps(xp().edid, xp().edidLen).scdc;   // it may be another monitor
	return true;
}

// One part of the generated table for the timing the pipe runs now.
bool RDNA4Device::runSecondPipePart(Pipe2::Part part, size_t *steps) {
	auto *plan = static_cast<Pipe2::Plan *>(IOMalloc(sizeof(Pipe2::Plan)));
	if (!plan)
		return false;
	Pipe2::Target t = xp().target;
	t.part = part;
	t.linkRate = xp().link.rate;
	const char *why = "";
	bool ok = Pipe2::build(t, *plan, &why);
	if (!ok)
		FBLOG("pipe2: no plan: %s", why);
	else
		ok = runSteps(plan->steps, plan->count, plan->cmds, plan->ncmds, "pipe2",
		              Pipe2::config().pipe * Pipe::Reg::kOtgStride, plan->aux);
	if (steps)
		*steps = plan->count;
	IOFree(plan, sizeof(Pipe2::Plan));
	return ok;
}

void RDNA4Device::setSecondPipePower(bool on) {
	if (!xp().lit || on == xp().on)
		return;
	// After a wake's power loss the cold start lights the pipe (coldStartHeads): before it nothing runs (no DMUB, the AUX
	// pads in their reset mode), and a wake tried then stopped with the stream encoder on, which the cold start's plan
	// then refused (the second display stayed dark, card, 2026-11-22).
	if (on && coldStartPending) {
		displayPowerNote(on, "pipe2", "left to the cold start after the power loss");
		return;
	}
	if (!displaySleepEnabled) {
		displayPowerNote(on, "pipe2", "ignored: rdna4-nosleep=1");
		return;
	}
	const Pipe2::Config &c = Pipe2::config();
	// The wake part trains the link: the smallest for the mode the pipe runs,
	// of the sink that is there now.
	if (on && c.dp && !pickSecondLink(xp().target.now.pixelClockKHz)) {
		displayPowerNote(on, "pipe2", "no DisplayPort link for the mode");
		return;
	}
	size_t steps = 0;
	const bool ok = runSecondPipePart(on ? Pipe2::Part::Wake : Pipe2::Part::Sleep, &steps);
	// The stream encoder's enable, and its symbol clock gate: amdgpu clears
	// the gate going to sleep and, for HDMI, never sets it again.
	const uint32_t fe = regReadDmu(2, Pipe::Reg::kDigFeEnCntl + c.dig * Pipe::Reg::kDigStride);
	const uint32_t symclk = regReadDmu(1, 0x00a0 + c.dig);                       // SYMCLKn_CLOCK_ENABLE
	FBLOG("power: pipe2 display %s: %lu steps %s; DIG%u_DIG_FE_EN_CNTL=0x%08x SYMCLK%c_CLOCK_ENABLE=0x%08x",
	      on ? "on" : "off", static_cast<unsigned long>(steps), ok ? "ran" : "STOPPED", c.dig, fe,
	      'A' + c.dig, symclk);
	char res[48];
	snprintf(res, sizeof(res), "%s, FE %08x SYMCLK %08x", ok ? "ran" : "STOPPED", fe, symclk);
	displayPowerNote(on, "pipe2", res);
	// A wake that stopped did not bring the display up: the next request to
	// turn it on, or the next plug, runs the wake again.
	xp().on = on && ok;
	if (xp().on) {
		scdcRefresh();
		diffAfterWake();
	}
}

void RDNA4Device::snapAfterLight(const Pipe2::Plan &plan)
{
	uint32_t on = 0;

	if (!Trial::option("rdna4-wakediff", on) || !on)
		return;

	if (!xp().snap)
		xp().snap = static_cast<RegSnap::Snap *>(IOMalloc(sizeof(RegSnap::Snap)));

	if (!xp().snap)
		return;

	RegSnap::take(*xp().snap, plan.steps, plan.count, snapRead, this);
	FBLOG("wakediff: %zu registers of pipe %u taken after lighting", xp().snap->count, xp().pipe);
}

void RDNA4Device::diffAfterWake()
{
	size_t differ;

	if (!xp().snap)
		return;

	gSnapLines = 0;
	differ = RegSnap::compare(*xp().snap, snapRead, snapDiff, this);
	FBLOG("wakediff: pipe %u after wake: %zu of %zu registers differ from after lighting", xp().pipe,
	      differ, xp().snap->count);
}

uint32_t RDNA4Device::snapRead(void *ctx, uint8_t seg, uint32_t dword)
{
	return static_cast<RDNA4Device *>(ctx)->regReadDmu(seg, dword);
}

void RDNA4Device::snapDiff(void *, const RegSnap::Reg &was, uint32_t now)
{
	if (gSnapLines++ < 60)
		FBLOG("wakediff:   %u:0x%04x %s: lit 0x%08x, now 0x%08x", was.seg, was.dword, was.what,
		      was.value, now);
}

/*
 * A scrambled link's TMDS_CONFIG once more, with the TMDS clock running: an
 * HDMI 2.0 sink forgets it while the clock is gone, and the wake wrote it
 * before the transmitter was on (the second Dell stayed dark after a wake
 * that had written it, 2026-11-04).
 */
void RDNA4Device::scdcRefresh()
{
	const uint32_t pixelKHz = xp().target.now.pixelClockKHz ? xp().target.now.pixelClockKHz :
								    Pipe2::config().timing.pixelClockKHz;
	const uint32_t khz = ModeSet::tmdsKHz(pixelKHz, Pipe2::config().y420 && !xp().target.rgb);

	if (Pipe2::config().dp || !xp().target.sinkScdc || khz <= ModeSet::kScrambleFromKHz)
		return;

	IOSleep(100);
	scdcConfigureWait(xp().target.ddcLine, static_cast<uint8_t>(ModeSet::scdcTmdsConfig(khz)));
}

// The modes the second pipe can be switched to scan the surface it was lit
// on, so none is larger than the plan's; and the sleep and wake parts of the
// table are for an unscrambled link.
Modes::Limits RDNA4Device::secondPipeModeLimits() const {
	const Edid::DetailedTiming &plan = Pipe2::config().timing;
	Modes::Limits lim {};
	lim.maxHActive = plan.hActive;
	lim.maxVActive = plan.vActive;
	// On DisplayPort, what the sink's largest link carries at 8 bpc.
	// On HDMI, TMDS without scrambling, or HDMI 2.0's 600 MHz for a pipe lit at
	// 3840x2160 (its sink has SCDC).
	lim.maxPixelClockKHz = Pipe2::config().dp
		? DpTrain::bandwidthKbps(DpTrain::Link { xp().sink.maxLanes, xp().sink.maxRate }) / 24
		: Pipe2::isUhd(Pipe2::config()) ? ModeSet::kMaxTmdsKHz : ModeSet::kScrambleFromKHz;

	// Behind a passive DP++ adapter, what the adapter carries: in 4:2:0
	// twice that in pixels.
	const uint32_t adaptorKHz = dpDualMaxKHz(Pipe2::config().hpd) * (Pipe2::config().y420 ? 2 : 1);
	if (adaptorKHz && adaptorKHz < lim.maxPixelClockKHz)
		lim.maxPixelClockKHz = adaptorKHz;

	return lim;
}

// Whether the pipe runs `t` in YCbCr 4:2:0: on a 4:2:0 table, a mode the
// adapter does not carry in RGB (amdgpu's force_yuv420_output).
bool RDNA4Device::secondPipeY420(const Edid::DetailedTiming &t) const
{
	const Pipe2::Config &c = Pipe2::config();

	return c.y420 && t.pixelClockKHz > dpDualMaxKHz(c.hpd);
}

// The mode's DET step held to `segments`, the pipe's share.
void RDNA4Device::clampDet(ModeSet::Step *steps, size_t n, uint32_t segments) const
{
	const Pipe2::Config &c = Pipe2::config();

	for (size_t i = 0; i < n; i++) {
		ModeSet::Step &s = steps[i];

		if (s.seg != c.detSeg || s.dword != c.detCtrl[c.pipe] || (s.value & 0x1f) <= segments)
			continue;

		s.value = (s.value & ~0x1fu) | segments;
		s.mask |= 0x1f;
	}
}

bool RDNA4Device::secondPipeModeOk(const Edid::DetailedTiming &t, const char **why) {
	const Pipe2::Config &c = Pipe2::config();
	const char *dummy;
	const char *&err = why ? *why : dummy;
	// Without Linux's request timing for the mode the pipe keeps the lit
	// mode's, which carries a pixel clock up to a quarter above its own: the
	// margin the boot display's switching runs with (buildModeTable).
	if (!Pipe2::modeKnown(t) && t.pixelClockKHz > c.timing.pixelClockKHz * 5 / 4) {
		err = "no request timing for a pixel clock this far above the lit mode's";
		return false;
	}

	// A pipe lit in YCbCr 4:2:0 runs in RGB what the adapter carries so, and
	// in 4:2:0 only formats the sink takes that way.
	if (secondPipeY420(t) && !Edid::takesY420(xp().edid, xp().edidLen, Edid::vicOf(t))) {
		err = "the adapter does not carry it in RGB, and the sink does not take it in YCbCr 4:2:0";
		return false;
	}

	// The DET segments the mode needs, or a smaller share as it is lit with.
	if (!secondPipeDet(t)) {
		err = "the DET buffer is too small for it next to the other pipes";
		return false;
	}
	return true;
}

/*
 * The DET segments the second pipe gets for `t` next to what the other pipes
 * and the compressed buffer hold: Linux's for the mode, or less as when it
 * is lit (detShare). With the boot display's plane still holding its 3
 * segments, Linux's 10 for 3840x2160 did not fit next to the other head's
 * 10, and a head lit at 3840x2160 with 7 could not be switched back to it
 * from a smaller mode.
 */
uint32_t RDNA4Device::secondPipeDet(const Edid::DetailedTiming &t) const
{
	constexpr uint32_t kCompbufCtrl = 0x04ba;   // as shrinkCompbuf: the compressed buffer's segments count too
	const Pipe2::Config &c = Pipe2::config();
	uint32_t others = regReadDmu(c.detSeg, kCompbufCtrl) >> 8 & 0x1f;

	for (uint32_t i = 0; i < 4; i++)
		if (i != c.pipe)
			others += regReadDmu(c.detSeg, c.detCtrl[i]) & 0x1f;

	return detShare(Pipe2::modeDetSegments(t), others);
}

// A mode that needs more of a DisplayPort link than it has: the link goes
// down and comes up larger, as for sleep and wake, with the stream still in
// the mode it runs. A link that is large enough stays.
bool RDNA4Device::relinkSecondPipe(uint32_t pixelClockKHz) {
	if (DpTrain::bandwidthKbps(xp().link) >= static_cast<uint64_t>(pixelClockKHz) * 24)
		return true;
	const DpTrain::Link had = xp().link;
	if (!pickSecondLink(pixelClockKHz))
		return false;
	const bool ok = runSecondPipePart(Pipe2::Part::Sleep) && runSecondPipePart(Pipe2::Part::Wake);
	if (!ok) {
		FBLOG("pipe2: the larger link did not come up: back to %u lane(s) at rate 0x%02x", had.lanes, had.rate);
		xp().link = had;
		runSecondPipePart(Pipe2::Part::Sleep);
		xp().on = runSecondPipePart(Pipe2::Part::Wake);
	}
	return ok;
}

IOReturn RDNA4Device::applySecondPipeMode(const Modes::Mode &m) {
	if (!xp().lit)
		return kIOReturnUnsupported;
	const Pipe2::Config &c = Pipe2::config();
	const uint32_t otgOffset = c.pipe * Pipe::Reg::kOtgStride;
	const char *why = "";
	if (!secondPipeModeOk(m.t, &why)) {
		FBLOG("pipe2: switch to id %u refused: %s", m.id, why);
		return kIOReturnUnsupported;
	}
	struct Work {
		ModeSet::Plan plan;
		ModeSet::Step extra[Pipe2::kMaxModeSteps];
	};
	auto *work = static_cast<Work *>(IOMalloc(sizeof(Work)));
	if (!work)
		return kIOReturnNoMemory;
	ModeSet::Plan *plan = &work->plan;
	ModeSet::Target t {};
	t.extra = work->extra;
	t.otg = t.opp = t.hubp = c.pipe;
	t.dig = c.dig;
	t.link = c.link;
	t.hpd = c.hpd;
	t.encoderObjId = xp().target.encoderObjId;
	t.connectorObjId = xp().connectorObjId;
	t.sinkScdc = xp().target.sinkScdc;
	t.ddcLine = xp().target.ddcLine;
	// DisplayPort: the stream is retimed on the link as it is, like the boot
	// display's, after the link was made large enough for the mode.
	const Edid::DetailedTiming was = xp().target.now;
	ModeSet::DpDto dtoNow {};
	if (c.dp && (!relinkSecondPipe(m.t.pixelClockKHz) || !readDpDto(c.pipe, dtoNow))) {
		FBLOG("pipe2: switch to id %u refused: no DisplayPort link or pixel-rate DTO for it", m.id);
		IOFree(work, sizeof(Work));
		return kIOReturnUnsupported;
	}
	t.dp = c.dp;
	t.dpMaxKHz = DpTrain::bandwidthKbps(xp().link) / 24;
	// From the timing the pipe runs to `to`, then the infoframe for it.
	bool touched = false;   // a refused plan changes nothing; one that stops half-way does
	auto retime = [&](const Edid::DetailedTiming &to) {
		t.from = xp().target.now;
		t.to = to;
		if (c.dp) {
			if (!ModeSet::scaleDpDto(dtoNow, was.pixelClockKHz, to.pixelClockKHz, t.dto))
				t.dto = {};
			t.vidM = ModeSet::dpVidM(to.pixelClockKHz, DpTrain::symbolClockKHz(xp().link.rate));
		}
		// A 4:2:0 table's encoding first, then the mode's registers.
		t.y420 = secondPipeY420(to);
		t.nextra = Pipe2::encodingSteps(t.y420, work->extra, Pipe2::kMaxModeSteps);
		t.nextra += Pipe2::modeSteps(to, work->extra + t.nextra, Pipe2::kMaxModeSteps - t.nextra);
		clampDet(work->extra, t.nextra, secondPipeDet(to));
		if (!ModeSet::build(t, *plan, &why))
			return false;
		why = "a step failed";
		touched = true;
		if (!runSteps(plan->steps, plan->count, plan->cmds, plan->ncmds, "pipe2", otgOffset))
			return false;
		xp().target.now = to;
		xp().target.rgb = c.y420 && !t.y420;
		return c.dp || runSecondPipePart(Pipe2::Part::Avi);
	};
	FBLOG("pipe2: switching to id %u %ux%u@%u.%03u (%u kHz), request timing %s", m.id, m.t.hActive, m.t.vActive,
	      m.refreshMilliHz / 1000, m.refreshMilliHz % 1000, m.t.pixelClockKHz,
	      Pipe2::modeKnown(m.t) ? "from Linux for this mode" : "of the lit mode");
	const bool ok = retime(m.t);
	if (!ok) {
		FBLOG("pipe2: switch to id %u failed: %s", m.id, why);
		if (touched && !retime(was))
			FBLOG("pipe2: the previous mode is NOT restored: %s", why);
	}
	const uint64_t periodNs = measureFramePeriodNs(otgOffset);
	const uint32_t mHz = periodNs ? static_cast<uint32_t>(1000000000000ULL / periodNs) : 0;
	FBLOG("pipe2: now %ux%u at %u kHz, OTG%u measured %u.%03u Hz; HUBP%u_DCHUBP_CNTL=0x%08x "
	      "ODM%u_OPTC_INPUT_GLOBAL_CONTROL=0x%08x DET %u", xp().target.now.hActive, xp().target.now.vActive,
	      xp().target.now.pixelClockKHz, c.pipe, mHz / 1000, mHz % 1000, c.pipe,
	      regReadDmu(2, 0x05f4 + c.pipe * Pipe::Reg::kHubpStride), c.pipe,
	      regReadDmu(2, 0x1aca + c.pipe * Pipe::Reg::kOdmStride), regReadDmu(c.detSeg, c.detCtrl[c.pipe]) & 0x1f);
	IOFree(work, sizeof(Work));

	if (ok) {
		// The sink's side of a scrambled link once more, with its clock
		// running, as after a wake.
		scdcRefresh();
		logUnderflow("pipe2: after the switch");
		diffAfterWake();
	}

	return ok ? kIOReturnSuccess : kIOReturnIOError;
}
