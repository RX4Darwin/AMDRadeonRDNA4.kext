//
//  device.cpp
//  RDNA4FB
//
//  Hardware side of RDNA4FB (see device.hpp). Moved out of the former
//  IOFramebuffer subclass unchanged in behaviour when RDNA4FB became a Lilu
//  plugin that extends Apple's IONDRVFramebuffer.
//

#include "device.hpp"
#include "edid.hpp"
#include "dmub.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>

#define FBLOG(fmt, ...)  IOLog("RDNA4FB: " fmt "\n", ## __VA_ARGS__)

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
	FBLOG("dcn:   DP0_DP_MSA_MISC = 0x%08x (MISC1..4)", regReadDmu(2, 0x2124));
	FBLOG("dcn:   DP0_DP_VID_STREAM_CNTL = 0x%08x", regReadDmu(2, 0x2122));
	FBLOG("dcn:   DP0_DP_SEC_CNTL  = 0x%08x (GSP/VSC/ASP enables)", regReadDmu(2, 0x2141));
	FBLOG("dcn:   DP0_DP_SEC_CNTL1 = 0x%08x", regReadDmu(2, 0x2142));
	FBLOG("dcn:   DP0_DP_SEC_CNTL2 = 0x%08x", regReadDmu(2, 0x2169));
	FBLOG("dcn:   DP0_DP_SEC_CNTL7 = 0x%08x", regReadDmu(2, 0x216e));
	FBLOG("dcn:   DP0_DP_MSA_VBID_MISC = 0x%08x", regReadDmu(2, 0x2170));

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

// AUX_REG_RW_CNTL_STATUS grant codes.
constexpr uint32_t kSwCanAccess   = 1;
constexpr uint32_t kDmcuCanAccess = 2;

// AUX reply nibble (first reply byte >> 4).
constexpr int kReplyAck      = 0x0;  // AUX ACK + I2C ACK
constexpr int kReplyAuxDefer = 0x2;
constexpr int kReplyI2CDefer = 0x8;

constexpr uint8_t kDdcSlave  = 0x50; // VESA DDC/EDID I2C address
constexpr uint8_t kAuxRetry  = 7;    // per-transaction defer retries
} // namespace

uint32_t RDNA4Device::auxDword(uint8_t inst, uint32_t reg) const {
	return kAuxBase0 + static_cast<uint32_t>(inst) * kAuxStride + reg;
}

int RDNA4Device::auxTransaction(uint8_t inst, uint8_t action, uint32_t address,
                               const uint8_t *data, uint8_t len,
                               uint8_t *reply, uint8_t replyCap,
                               uint8_t *replyBytes) {
	if (replyBytes) *replyBytes = 0;
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
	if (((arb & kAuxRwStatMask) >> kAuxRwStatShift) == kDmcuCanAccess)
		return -1;  // the firmware microcontroller owns this engine
	uint32_t ctl = regReadDmu(2, rCtl);
	if (!(ctl & kAuxEn))                       // GOP normally leaves AUX enabled
		regWriteDmu(2, rCtl, ctl | kAuxEn);
	regWriteDmu(2, rArb, regReadDmu(2, rArb) | kAuxUseReq);
	arb = regReadDmu(2, rArb);
	if (((arb & kAuxRwStatMask) >> kAuxRwStatShift) != kSwCanAccess) {
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
	if (!done || (sts & kAuxHpdDiscon) ||
	    (sts & kAuxRxTimeout) || (sts & kAuxRxTimeoutState)) {
		release();
		return -1;
	}

	uint32_t nbytes = (sts & kAuxReplyCountMask) >> kAuxReplyCountShift;

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
                          uint8_t start) {
	uint8_t rb = 0;

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
		const char *bus = viaAux ? "AUX" : "DDC";

		uint8_t edid[128] {};
		bool got = viaAux ? readEDID(inst, edid, sizeof(edid), 0)
		                  : readEDIDI2C(inst, edid, sizeof(edid), 0);
		if (!got) {
			FBLOG("edid: connector %zu (%s%u): no reply", i, bus, inst);
			continue;
		}

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

		// Cache the first sink's EDID for hasDDCConnect()/getDDCBlock(). Only
		// the boot display is scanned out, and that is AUX0/DP0 on this board.
		if (edidLen == 0) {
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
		}
	}
	if (!any)
		FBLOG("edid: no sink EDID read on any connector");
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
} // namespace

bool RDNA4Device::readEDIDI2C(uint8_t line, uint8_t *edid, size_t count,
                             uint8_t start) {
	if (!ipDiscovery.isValid() || !rmmio || count == 0 || count > 256)
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

	// --- queue both transactions, then a single GO (process_transaction /
	// execute_transaction): [START 0xA0 <start>] [START 0xA1 read*count STOP]
	regWriteDmu(2, kI2cTxn0,
	            kTxnStopOnNack | kTxnStart | (1u << kTxnCountShift));
	regWriteDmu(2, kI2cTxn0 + 1,
	            kTxnStopOnNack | kTxnStart | kTxnRead | kTxnStop |
	            (static_cast<uint32_t>(count) << kTxnCountShift));
	// Data FIFO: address bytes carry the R/W bit in bit 0.
	regWriteDmu(2, kI2cDataReg, kI2cIndexWrite | (0xA0u << kI2cDataShiftI2C));
	regWriteDmu(2, kI2cDataReg, static_cast<uint32_t>(start) << kI2cDataShiftI2C);
	regWriteDmu(2, kI2cDataReg, 0xA1u << kI2cDataShiftI2C);

	uint32_t ctl = (static_cast<uint32_t>(line) << kI2cDdcSelectShift) |
	               (1u << kI2cTxnCountShift);  // TRANSACTION_COUNT = 2-1
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
	// after the 3 bytes we wrote (0xA0, start, 0xA1).
	regWriteDmu(2, kI2cDataReg,
	            kI2cIndexWrite | kI2cDataRead | (3u << kI2cIndexShift));
	for (size_t i = 0; i < count; i++)
		edid[i] = static_cast<uint8_t>(
		    (regReadDmu(2, kI2cDataReg) >> kI2cDataShiftI2C) & 0xff);

	release();
	return true;
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
uint64_t RDNA4Device::measureFramePeriodNs() {
	if (!pipe.valid())
		return 0;
	constexpr uint32_t kFrames = 10;
	const uint32_t reg = Pipe::Reg::kOtgFrameCount + otgOff();

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
constexpr uint32_t kDmcubInbox1Size        = 0x01d5;
constexpr uint32_t kDmcubInbox1Wptr        = 0x01d6;
constexpr uint32_t kDmcubInbox1Rptr        = 0x01d7;
constexpr uint32_t kDcnVmFbLocationBase    = 0x0475;   // VRAM MC base, 16 MiB units
} // namespace

bool RDNA4Device::dmubRing(DmubRing &ring) {
	ring = DmubRing {};
	if (!ipDiscovery.isValid() || !rmmio)
		return false;
	// The ring lives in top-of-VRAM firmware memory (REGION4); its VRAM
	// position is the REGION4 MC address minus the VRAM MC base.
	uint64_t region4Mc = regReadDmu(2, kDmcubRegion4Offset) |
	    (static_cast<uint64_t>(regReadDmu(2, kDmcubRegion4OffsetHigh)) << 32);
	uint64_t vramMcBase =
	    static_cast<uint64_t>(regReadDmu(2, kDcnVmFbLocationBase) & 0xffffff) << 24;
	if (region4Mc <= vramMcBase) {
		FBLOG("dmub: region4 mc 0x%llx not above vram base 0x%llx", region4Mc, vramMcBase);
		return false;
	}
	uint32_t size = regReadDmu(2, kDmcubInbox1Size);
	if (size == 0 || size > 0x100000 || (size % Dmub::kCmdSize) != 0) {
		FBLOG("dmub: inbox1 size 0x%x not sane", size);
		return false;
	}
	ring.base = region4Mc - vramMcBase;
	ring.size = size;
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
		if (nrptr == slot)
			return true;
		IODelay(10);
	}
	FBLOG("%s: firmware did not consume (wptr=0x%x rptr stuck at 0x%x)", tag, slot, nrptr);
	return false;
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

void RDNA4Device::setDisplayPower(bool on) {
	if (!displaySleepEnabled || on == displayPowerOn)
		return;
	if (!ipDiscovery.isValid() || !rmmio)
		return;

	// HDMI/DVI boot pipe: there is no DP stream or DPCD to toggle. Blank to
	// solid black with the OPP's display pattern generator, the way
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
		return;
	}

	if (on) {
		// Wake the sink first so it sees video the moment the stream returns.
		bool acked = sinkPower(0x1);
		regWriteDmu(2, kDpVidStreamCntl, v | kVidStreamEnable);
		FBLOG("power: display on (sink D0 %s, stream 0x%08x -> 0x%08x)",
		      acked ? "acked" : "no ack", v, regReadDmu(2, kDpVidStreamCntl));
	} else {
		// Blank the stream, then let the sink drop to D3. The timing
		// generator keeps running; only the video stream enable is touched.
		regWriteDmu(2, kDpVidStreamCntl, v & ~kVidStreamEnable);
		bool acked = sinkPower(0x2);
		FBLOG("power: display off (stream 0x%08x -> 0x%08x, sink D3 %s)",
		      v, regReadDmu(2, kDpVidStreamCntl), acked ? "acked" : "no ack");
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

	// DP0 stream encoder (the active one on this machine).
	constexpr uint32_t kDpPixelFormat   = 0x211f; // base 2
	constexpr uint32_t kDpMsaColorimetry= 0x2120; // base 2
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
	FBLOG("8bpc: DP_PIXEL_FORMAT 0x%08x -> 0x%08x, MSA_COLORIMETRY 0x%08x -> 0x%08x",
	      pf, pfNew, col, colNew);
	regWriteDmu(2, kDpPixelFormat, pfNew);
	regWriteDmu(2, kDpMsaColorimetry, colNew);
	FBLOG("8bpc: readback DP_PIXEL_FORMAT=0x%08x MSA_COLORIMETRY=0x%08x",
	      regReadDmu(2, kDpPixelFormat), regReadDmu(2, kDpMsaColorimetry));
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
	hwCursorRequested = PE_parse_boot_argn("rdna4-cursor", &cursor, sizeof(cursor)) && cursor != 0;
	if (hwCursorRequested)
		FBLOG("cursor: hardware cursor requested by rdna4-cursor=1");
	const char *model = (vd >> 16) == 0x7551 ? "AMD Radeon AI PRO R9700"
	                                         : "AMD Radeon RX 9070 XT";
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
			dumpDCN();
			// DP-stream experiment; meaningless (and aimed at DP0) on HDMI.
			if (!pipe.isTmds())
				tryForce8bpc();
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

	// Needs the boot timing (discoverPipe) and the sink's EDID (probeEDID).
	buildModeTable();
	FBLOG("init: device ready (%ux%u console, %lu mode(s))", fbWidth, fbHeight,
	      static_cast<unsigned long>(modeCount));
	return true;
}

RDNA4Device::~RDNA4Device() {
	freeHardwareCursor();
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

#ifdef RDNA4FB_VM_TEST
// Lenovo G25-10 base block (the tools/atomdump.cpp fixture). With boot-arg
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
#ifdef RDNA4FB_VM_TEST
	// VM test builds exercise the mode table by default (OpenCore images
	// often pin boot-args, so opt-out rather than opt-in); "=0" disables.
	uint32_t ms = 1, fake = 1;
#else
	uint32_t ms = 0;
#endif
	PE_parse_boot_argn("rdna4-modeset", &ms, sizeof(ms));
	modesetRequested = ms != 0;

#ifdef RDNA4FB_VM_TEST
	PE_parse_boot_argn("rdna4-fakeedid", &fake, sizeof(fake));
	if (!edidLen && fake) {
		memcpy(edidData, kVmFixtureEdid, sizeof(kVmFixtureEdid));
		edidLen = sizeof(kVmFixtureEdid);
		FBLOG("modes: VM test build: serving the Lenovo fixture EDID");
	}
#endif

	// Modes reuse the GOP framebuffer's memory and pitch, so none may be
	// larger than it. TMDS stays below 340 MHz (no HDMI 2.0 scrambling yet)
	// and within 25 % of the boot pixel clock, which the GOP's DISPCLK is
	// known to carry, until DISPCLK is read and raised explicitly.
	Modes::Limits lim {};
	lim.maxHActive = fbWidth;
	lim.maxVActive = fbHeight;
	lim.maxPixelClockKHz = 340000;
	if (bootTimingValid && bootTiming.pixelClockKHz && bootTiming.pixelClockKHz * 5 / 4 < 340000)
		lim.maxPixelClockKHz = bootTiming.pixelClockKHz * 5 / 4;

	if (modesetRequested && edidLen)
		modeCount = Modes::build(edidData, edidLen, lim, modeTable, Modes::MaxModes);

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
	      modesetRequested ? "requested (rdna4-modeset=1)" : "off (boot mode only)");
	owner->setProperty("Modes,Count", static_cast<uint64_t>(modeCount), 32);
}

const Modes::Mode *RDNA4Device::findMode(uint32_t id) const {
	for (size_t i = 0; i < modeCount; i++)
		if (modeTable[i].id == id)
			return &modeTable[i];
	return nullptr;
}

// The VBIOS display path wired to the lit pipe: the one whose HPD pin is
// the back-end's DIG_HPD_SELECT.
bool RDNA4Device::pathForPipe(AtomBios::DisplayPath &out) {
	AtomBios::DisplayPath paths[AtomBios::MaxDisplayPaths];
	size_t n = atomBios.getDisplayPaths(paths, AtomBios::MaxDisplayPaths);
	for (size_t i = 0; i < n; i++) {
		AtomBios::PathRecords rec;
		if (atomBios.getPathRecords(paths[i], rec) && rec.hasHpd && rec.hpdPin == pipe.hpd) {
			out = paths[i];
			return true;
		}
	}
	return false;
}

// Let `frames` frames of the lit OTG pass, by its frame counter.
bool RDNA4Device::waitFrames(uint32_t frames) {
	const uint32_t reg = Pipe::Reg::kOtgFrameCount + otgOff();
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

bool RDNA4Device::runPlan(const ModeSet::Plan &plan) {
	for (size_t i = 0; i < plan.count; i++) {
		const ModeSet::Step &s = plan.steps[i];
		switch (s.op) {
		case ModeSet::Op::Write:
			regWriteDmu(s.seg, s.dword, s.value);
			break;
		case ModeSet::Op::Update:
			regWriteDmu(s.seg, s.dword, (regReadDmu(s.seg, s.dword) & ~s.mask) | s.value);
			break;
		case ModeSet::Op::WaitSet:
		case ModeSet::Op::WaitClear: {
			bool ok = false;
			for (uint32_t us = 0;; us += 10) {
				uint32_t v = regReadDmu(s.seg, s.dword) & s.mask;
				if (s.op == ModeSet::Op::WaitSet ? v == s.mask : v == 0) {
					ok = true;
					break;
				}
				if (us >= s.arg)
					break;
				IODelay(10);
			}
			if (!ok) {
				FBLOG("modeset: step %lu (%s) timed out%s", static_cast<unsigned long>(i),
				      s.what, s.optional ? ", continuing" : "");
				if (!s.optional)
					return false;
			}
			break;
		}
		case ModeSet::Op::Dmub:
			if (s.arg >= plan.ncmds || !dmubSubmit(&plan.cmds[s.arg], 1, "modeset")) {
				FBLOG("modeset: step %lu (%s): DMUB did not take the command",
				      static_cast<unsigned long>(i), s.what);
				return false;
			}
			break;
		case ModeSet::Op::WaitFrames:
			if (!waitFrames(s.arg)) {
				FBLOG("modeset: step %lu (%s): the OTG is not counting frames",
				      static_cast<unsigned long>(i), s.what);
				return false;
			}
			break;
		}
	}
	return true;
}

IOReturn RDNA4Device::applyMode(const Modes::Mode &m) {
	if (!pipe.valid() || !pipe.isTmds() || !liveTimingValid || !rmmio || !ipDiscovery.isValid()) {
		FBLOG("modes: switch to id %u refused: no programmable HDMI pipe", m.id);
		return kIOReturnUnsupported;
	}
	AtomBios::DisplayPath path {};
	if (!pathForPipe(path)) {
		FBLOG("modes: switch to id %u refused: no VBIOS path for HPD%u", m.id, pipe.hpd);
		return kIOReturnUnsupported;
	}

	ModeSet::Target t {};
	t.otg = pipe.otg;
	t.dig = pipe.dig;
	t.link = pipe.link;
	t.hpd = pipe.hpd;
	t.opp = pipe.opp;
	t.hubp = pipe.hubp;
	t.encoderObjId = path.encoderObjId;
	t.connectorObjId = path.connectorObjId;
	t.pllId = 0x14;                  // ATOM_COMBOPHY_PLL0, amdgpu's first free PLL
	t.from = liveTiming;
	t.to = m.t;
	const char *why = "";
	if (!ModeSet::build(t, modePlan, &why)) {
		FBLOG("modes: switch to id %u refused: %s", m.id, why);
		return kIOReturnUnsupported;
	}
	FBLOG("modes: switching to id %u %ux%u@%u.%03u (%u kHz, vstartup %u): %lu steps, "
	      "%lu DMUB commands", m.id, m.t.hActive, m.t.vActive, m.refreshMilliHz / 1000,
	      m.refreshMilliHz % 1000, m.t.pixelClockKHz, ModeSet::vstartupLines(m.t),
	      static_cast<unsigned long>(modePlan.count), static_cast<unsigned long>(modePlan.ncmds));
	if (runPlan(modePlan)) {
		liveTiming = m.t;
		FBLOG("modes: now %ux%u@%u.%03u", m.t.hActive, m.t.vActive,
		      m.refreshMilliHz / 1000, m.refreshMilliHz % 1000);
		return kIOReturnSuccess;
	}

	// Put the previous timing back so the display is not left dark.
	t.from = m.t;
	t.to = liveTiming;
	bool restored = ModeSet::build(t, modePlan, &why) && runPlan(modePlan);
	FBLOG("modes: switch to id %u failed, previous mode %s", m.id,
	      restored ? "restored" : "NOT restored");
	return kIOReturnIOError;
}

