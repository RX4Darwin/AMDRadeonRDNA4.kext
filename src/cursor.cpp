//
//  cursor.cpp
//  RDNA4FB
//
//  PARKED — not built. The DCN hardware-cursor code from the former
//  IOFramebuffer subclass (setCursorImage/setCursorState via
//  convertCursorImage) plus the DMUB cursor experiment. Under the Lilu/NDRV
//  design the cursor arrives through IONDRVFramebuffer's NDRV cursor path
//  (cscSupportsHardwareCursor / cscSetHardwareCursor / cscDrawHardwareCursor
//  with VSLPrepareCursorForHardwareCursor); this is the register-level half
//  to wire in when that path is implemented.
//

#if 0

// ---------------------------------------------------------------------------
// Hardware cursor (DCN cursor plane, pipe 0)
// ---------------------------------------------------------------------------

namespace {
// dcn_4_1_0_offset.h, all base_idx 2. HUBP-side CURSOR0_0 block plus the
// DPP-side CM_CUR0 enable.
constexpr uint32_t kCurControl  = 0x0679;
constexpr uint32_t kCurAddr     = 0x067a;
constexpr uint32_t kCurAddrHigh = 0x067b;
constexpr uint32_t kCurSize     = 0x067c;
constexpr uint32_t kCurPosition = 0x067d;
constexpr uint32_t kCurHotSpot  = 0x067e;
constexpr uint32_t kCurDstOffset = 0x0680;
constexpr uint32_t kCmCur0Control = 0x0cf1;
// DCN 4.01 cursor FP pipeline (new on this generation): cursor pixels are
// multiplied by an FP16 scale before blending. The GOP never uses a cursor
// and leaves scale at 0.0 — every register and the sprite data verify
// perfectly while the cursor renders as nothing. 0x3c00 is FP16 1.0, the
// default from dcn10_set_cursor_sdr_white_level; bias 0; matrix bypass (0).
constexpr uint32_t kCmCur0FpScaleBiasGY = 0x0cf4;  // SCALE [15:0], BIAS [31:16]
constexpr uint32_t kCmCur0FpScaleBiasRB = 0x0cf5;
constexpr uint32_t kCmCur0MatrixMode    = 0x0cf6;
constexpr uint32_t kCurFpScaleOne       = 0x3c00;  // FP16 1.0
// Pipe update latching. Cursor/CM registers are double-buffered: writes go
// to a pending copy that latches into live hardware at the frame boundary —
// but only while OTG_MASTER_UPDATE_LOCK is released. The GOP programs its
// pipe under the lock and has no reason to ever release it, which freezes
// every later pipe update in pending space (reads return the pending values,
// so all writes "verify" while the hardware never changes). LOCK bit 0 is
// the request; UPDATE_LOCK_STATUS bit 8 and OTG_UPDATE_PENDING bit 0 of
// DOUBLE_BUFFER_CONTROL are true status bits.
constexpr uint32_t kOtgMasterUpdateLock  = 0x1b89;
constexpr uint32_t kOtgDoubleBufferCtl   = 0x1b5c;
constexpr uint32_t kDppTopControl        = 0x0cc5;
// Global sync: pipe updates latch on the VUPDATE pulse, which is a
// PROGRAMMABLE event (offset/width lines, positioned via VSTARTUP). amdgpu
// programs it at every modeset; a GOP that never updates its pipe again has
// no reason to program a pulse at all — width 0 means the latch event never
// fires and every double-buffered write stays pending forever. This also
// retroactively explains the earlier 8bpc/MCM writes that read back changed
// but never altered the picture. GLOBAL_SYNC_STATUS bit 8
// (VUPDATE_EVENT_OCCURRED) is the ground truth.
constexpr uint32_t kOtgVStartupParam     = 0x1b85;
constexpr uint32_t kOtgVUpdateParam      = 0x1b86;
constexpr uint32_t kOtgVReadyParam       = 0x1b87;
constexpr uint32_t kOtgGlobalSyncStatus  = 0x1b88;
// HUBPREQ0_CURSOR_SETTINGS — cursor fetch scheduling. amdgpu always programs
// CHUNK_HDL_ADJUST=3 ([9:8]); without it and a correct LINES_PER_CHUNK the
// cursor request pipeline can fetch nothing (sprite armed but invisible).
constexpr uint32_t kCurSettings = 0x0653;
constexpr uint32_t kCurChunkHdlAdjust = 3u << 8;

// Field layout (dcn_4_1_0_sh_mask.h): CURSOR_CONTROL enable bit0, REQ_MODE
// bit2, MODE [9:8], PITCH [17:16], LINES_PER_CHUNK [25:24]; SIZE height
// [15:0] width [31:16]; POSITION y [14:0], x starts at bit 15 (NOT 16 on
// this generation).
// CURSOR_REQ_MODE=1 (fetch during display prefetch) is MANDATORY on DCN4x:
// per dcn401_hubp.c, mode 0 (legacy fetch-just-in-time) "is no longer
// supported" — with it the cursor simply never fetches (hardware-confirmed:
// three rounds of perfect registers + verified sprite data, no pixels).
constexpr uint32_t kCurReqMode    = 1u << 2;
constexpr uint32_t kCurModeShift  = 8;
constexpr uint32_t kCurPitchShift = 16;
constexpr uint32_t kCurLpcShift   = 24;
constexpr uint32_t kCurXShift     = 15;
// CM_CUR0_CURSOR0_CONTROL: CUR0_ENABLE bit0, CUR0_MODE [6:4], plus two bits
// captured from a WORKING amdgpu cursor on this exact GPU (register diff,
// Debian, 2026-07-12: working value 0x000000a5 vs our 0x21): bit 7
// (CUR0_PIXEL_ALPHA_MOD_EN) and bit 2 (CUR0_PIX_INV_MODE per the sh_mask).
// Neither is set by dpp401_set_cursor_attributes — amdgpu programs them in
// a layer we had not ported. Mirror the proven-working value.
constexpr uint32_t kCur0ModeShift   = 4;
constexpr uint32_t kCur0WorkingBits = (1u << 7) | (1u << 2);

// hubp1_get_lines_per_chunk for color cursors: enum {1,2,4,8,16} lines
// encodes as 0..4.
static uint32_t cursorLinesPerChunk(uint32_t width) {
	if (width <= 32)  return 4;  // 16 lines
	if (width <= 64)  return 3;  // 8 lines
	if (width <= 128) return 2;  // 4 lines
	return 1;                    // 2 lines
}

// HUBPREQ0 scanout address registers — anchor for the sprite's GPU address.
constexpr uint32_t kHubpPrimaryAddr     = 0x060a;
constexpr uint32_t kHubpPrimaryAddrHigh = 0x060b;

constexpr uint32_t kCursorPixels = 128;               // fixed 128x128 slot
constexpr uint32_t kCursorPitchCode = 1;              // 0=64,1=128,2=256 px
constexpr uint32_t kCursorBytes  = kCursorPixels * kCursorPixels * 4;
} // namespace


bool RDNA4Device::initHWCursor() {
	if (!hwCursorRequested || !ipDiscovery.isValid() || !rmmio ||
	    fbPhysBase == 0 || fbLength == 0)
		return false;

	uint32_t lo = regReadDmu(2, kHubpPrimaryAddr + hubpOff());
	uint32_t hi = regReadDmu(2, kHubpPrimaryAddrHigh + hubpOff());
	if (lo == 0xFFFFFFFF || (lo == 0 && hi == 0)) {
		FBLOG("cursor: scanout MC address unreadable, staying with software cursor");
		return false;
	}
	uint64_t scanoutMc = (static_cast<uint64_t>(hi & 0xffff) << 32) | lo;
	scanoutMcAddr = scanoutMc;

	uint32_t ctest = 0;
	if (PE_parse_boot_argn("rdna4-curtest", &ctest, sizeof(ctest)) && ctest != 0) {
		hwCursorTest = true;
		FBLOG("cursor: TEST MODE: sprite will fetch from the scanout base");
	}

	// Address-routing state (read-only): the DCHUBBUB FB/AGP windows and the
	// per-HUBP system aperture decide where cursor memory requests actually
	// go. Out-of-window requests return zeros without latching any error.
	FBLOG("cursor: vm: fb_loc base=0x%08x top=0x%08x agp base=0x%08x "
	      "bot=0x%08x top=0x%08x sys_ap=0x%08x/0x%08x l1_tlb=0x%08x",
	      regReadDmu(2, 0x0475), regReadDmu(2, 0x0476),
	      regReadDmu(2, 0x047a), regReadDmu(2, 0x0478), regReadDmu(2, 0x0479),
	      regReadDmu(2, 0x062c + hubpOff()), regReadDmu(2, 0x062d + hubpOff()),
	      regReadDmu(2, 0x063a + hubpOff()));

	// Sprite right after the framebuffer, 8 KiB aligned. The CPU sees VRAM
	// through the BAR at the same linear offsets as the MC addresses, so the
	// CPU-visible sprite address is fbPhysBase plus the identical delta.
	cursorMcAddr = (scanoutMc + fbLength + 0x1FFFULL) & ~0x1FFFULL;
	uint64_t delta = cursorMcAddr - scanoutMc;
	// Stay well inside the 256 MiB non-ReBAR VRAM window.
	if (delta + kCursorBytes > 192ULL * 1024 * 1024) {
		FBLOG("cursor: sprite offset 0x%llx outside safe aperture window", delta);
		return false;
	}

	IODeviceMemory *mem = IODeviceMemory::withRange(fbPhysBase + delta, kCursorBytes);
	if (!mem)
		return false;
	cursorMap = mem->map();
	mem->release();
	if (!cursorMap) {
		FBLOG("cursor: failed to map sprite VRAM");
		return false;
	}
	cursorVram = reinterpret_cast<volatile uint32_t *>(cursorMap->getVirtualAddress());

	cursorStage = static_cast<uint32_t *>(IOMalloc(kCursorBytes));
	if (!cursorStage) {
		cursorMap->release();
		cursorMap = nullptr;
		cursorVram = nullptr;
		return false;
	}

	// Double-buffered cursor/CM writes only go live once the GOP's update
	// lock is released and a VUPDATE pulse exists.
	ensureUpdateLatch();

	FBLOG("cursor: HW cursor armed: scanout MC 0x%llx, sprite MC 0x%llx "
	      "(cpu 0x%llx), mode %u", scanoutMc, cursorMcAddr,
	      fbPhysBase + delta, hwCursorMode);
	hwCursorReady = true;
	return true;
}

IOReturn RDNA4Device::setCursorImage(void *cursorImage) {
	if (!hwCursorReady)
		return kIOReturnUnsupported;

	IOHardwareCursorDescriptor desc {};
	desc.majorVersion = kHardwareCursorDescriptorMajorVersion;
	desc.minorVersion = kHardwareCursorDescriptorMinorVersion;
	desc.height   = kCursorPixels;
	desc.width    = kCursorPixels;
	desc.bitDepth = 32;   // direct ARGB

	IOHardwareCursorInfo info {};
	info.majorVersion = kHardwareCursorInfoMajorVersion;
	info.minorVersion = kHardwareCursorInfoMinorVersion;
	info.hardwareCursorData = reinterpret_cast<UInt8 *>(cursorStage);

	if (!convertCursorImage(cursorImage, &desc, &info))
		return kIOReturnUnsupported;
	uint32_t w = info.cursorWidth, h = info.cursorHeight;
	if (w == 0 || h == 0 || w > kCursorPixels || h > kCursorPixels)
		return kIOReturnUnsupported;

	// Staging buffer is tightly packed (w*4); the VRAM slot has a fixed
	// 128-pixel pitch. Clear the slot so stale pixels never show at edges.
	for (uint32_t i = 0; i < kCursorPixels * kCursorPixels; i++)
		cursorVram[i] = 0;
	for (uint32_t row = 0; row < h; row++)
		for (uint32_t col = 0; col < w; col++)
			cursorVram[row * kCursorPixels + col] = cursorStage[row * w + col];

	// Data-integrity check for the first images: find an opaque staging
	// pixel and read the same location back from VRAM. Registers verifying
	// while the pointer stays invisible means either the conversion made a
	// fully transparent sprite (staging max-alpha 0) or the CPU->VRAM window
	// is not where we think (VRAM readback mismatch).
	if (cursorImgLogs < 3) {
		uint32_t opaqueIdx = 0, opaqueVal = 0;
		for (uint32_t i = 0; i < w * h; i++) {
			if ((cursorStage[i] >> 24) > (opaqueVal >> 24)) {
				opaqueVal = cursorStage[i];
				opaqueIdx = i;
			}
			if ((opaqueVal >> 24) == 0xff)
				break;
		}
		uint32_t row = opaqueIdx / w, col = opaqueIdx % w;
		uint32_t vramVal = cursorVram[row * kCursorPixels + col];
		FBLOG("cursor: pixel check: stage[%u,%u]=0x%08x vram=0x%08x %s",
		      col, row, opaqueVal, vramVal,
		      opaqueVal == vramVal ? "(match)" : "(MISMATCH - aperture wrong)");
	}

	uint64_t spriteAddr = hwCursorTest ? scanoutMcAddr : cursorMcAddr;
	regWriteDmu(2, kCurAddrHigh + hubpOff(), static_cast<uint32_t>(spriteAddr >> 32) & 0xffff);
	regWriteDmu(2, kCurAddr + hubpOff(), static_cast<uint32_t>(spriteAddr));
	regWriteDmu(2, kCurSize + hubpOff(), h | (w << 16));
	// setCursorState receives hotspot-adjusted top-left coords, so the
	// hardware hotspot stays zero.
	regWriteDmu(2, kCurHotSpot + hubpOff(), 0);
	// Fetch scheduling (missing on first hardware try — sprite armed but
	// invisible): chunk handle deadline adjust + lines per fetch chunk.
	regWriteDmu(2, kCurSettings + hubpOff(), kCurChunkHdlAdjust);
	cursorCtlBase = (cursorLinesPerChunk(w) << kCurLpcShift) |
	                (kCursorPitchCode << kCurPitchShift) |
	                (hwCursorMode << kCurModeShift) |
	                kCurReqMode;
	regWriteDmu(2, kCurControl + hubpOff(), cursorCtlBase | (hwCursorVisible ? 1u : 0u));
	// The FP scale stage: without FP16 1.0 here the sprite is multiplied
	// to invisibility (found on hardware 2026-07-12).
	regWriteDmu(2, kCmCur0FpScaleBiasGY + dppOff(), kCurFpScaleOne);
	regWriteDmu(2, kCmCur0FpScaleBiasRB + dppOff(), kCurFpScaleOne);
	regWriteDmu(2, kCmCur0MatrixMode + dppOff(), 0);   // matrix bypass
	regWriteDmu(2, kCmCur0Control + dppOff(),
	            (hwCursorMode << kCur0ModeShift) | kCur0WorkingBits |
	            (hwCursorVisible ? 1u : 0u));

	if (cursorImgLogs < 3) {
		cursorImgLogs++;
		FBLOG("cursor: image %ux%u px0=0x%08x rb: ctl=0x%08x size=0x%08x "
		      "addr=0x%08x/%04x set=0x%08x cm=0x%08x fp=0x%08x/0x%08x "
		      "hubp_cntl=0x%08x cnvc=0x%08x/0x%08x",
		      w, h, cursorStage[0],
		      regReadDmu(2, kCurControl + hubpOff()), regReadDmu(2, kCurSize + hubpOff()),
		      regReadDmu(2, kCurAddr + hubpOff()), regReadDmu(2, kCurAddrHigh + hubpOff()) & 0xffff,
		      regReadDmu(2, kCurSettings + hubpOff()), regReadDmu(2, kCmCur0Control + dppOff()),
		      regReadDmu(2, kCmCur0FpScaleBiasGY + dppOff()),
		      regReadDmu(2, kCmCur0FpScaleBiasRB + dppOff()),
		      regReadDmu(2, 0x05f4 + hubpOff()),           // HUBP0_DCHUBP_CNTL
		      regReadDmu(2, 0x0ccf + dppOff()),           // CNVC surface pixel format
		      regReadDmu(2, 0x0cd0 + dppOff()));          // CNVC format control
	}
	return kIOReturnSuccess;
}

IOReturn RDNA4Device::setCursorState(SInt32 x, SInt32 y, bool visible) {
	if (!hwCursorReady)
		return kIOReturnUnsupported;

	// Signed coords can go negative when the pointer overlaps the top/left
	// edge; clamp (v1 accepts the sprite pinning at the edge there).
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	regWriteDmu(2, kCurPosition + hubpOff(),
	            (static_cast<uint32_t>(y) & 0x7fff) |
	            (static_cast<uint32_t>(x) << kCurXShift));
	// Cursor fetch deadline (hubp2_cursor_set_position): the source x offset
	// scaled from pixel time to refclk time. 100 MHz refclk over the 533.25
	// MHz boot pixel clock; precision is uncritical (it is a deadline hint).
	regWriteDmu(2, kCurDstOffset + hubpOff(),
	            (static_cast<uint32_t>(x) * 100000u) / livePixelClockKHz());
	// Only touch the double-buffered control registers on visibility
	// changes — rewriting them every move re-arms UPDATE_PENDING and hides
	// whether latching ever completes.
	if (visible != hwCursorVisible) {
		if (cursorCtlBase)   // 0 until the first setCursorImage
			regWriteDmu(2, kCurControl + hubpOff(), cursorCtlBase | (visible ? 1u : 0u));
		regWriteDmu(2, kCmCur0Control + dppOff(),
		            (hwCursorMode << kCur0ModeShift) | kCur0WorkingBits |
		            (visible ? 1u : 0u));
	}
	hwCursorVisible = visible;

	// Log the first few calls, then a sparse sample of later ones — the
	// later samples show whether CUR0_UPDATE_PENDING (cm bit 16) ever
	// clears and whether VUPDATE events occur (sync bit 8) once the
	// control registers are left alone between visibility changes.
	cursorPosCalls++;
	if (cursorPosLogs < 6 ||
	    (cursorPosLogs < 14 && (cursorPosCalls & 0x1ff) == 0)) {
		cursorPosLogs++;
		FBLOG("cursor: state#%u x=%d y=%d vis=%d rb: pos=0x%08x ctl=0x%08x "
		      "cm=0x%08x sync=0x%08x",
		      cursorPosCalls, (int)x, (int)y, visible,
		      regReadDmu(2, kCurPosition + hubpOff()), regReadDmu(2, kCurControl + hubpOff()),
		      regReadDmu(2, kCmCur0Control + dppOff()),
		      regReadDmu(2, kOtgGlobalSyncStatus + otgOff()));
	}
	return kIOReturnSuccess;
}


// rdna4-dmubcursor=1: the flanking move for the invisible-cursor saga —
// hand the firmware our cursor register images via DMUB_CMD__UPDATE_CURSOR_
// INFO (two chained ring entries, dc_send_update_cursor_info_to_dmu) and let
// IT program the hardware. A white 64x64 square appearing at (100,100)
// means AMD's own code can light the cursor plane where nine rounds of
// direct programming could not; nothing appearing is decisive the other way.
// Note: the DMUB position layout is x[15:0], y[31:16] — the OPPOSITE of the
// dcn_4_1_0_sh_mask claim; whichever the FW writes is the silicon truth.
void RDNA4Device::dmubCursorTest() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("rdna4-dmubcursor", &on, sizeof(on)) || on == 0)
		return;
	if (!hwCursorReady || !cursorVram) {
		FBLOG("dmub: cursor test needs rdna4-hwcursor=1 (sprite slot)");
		return;
	}

	// Opaque white 64x64 sprite in the (128-pixel-pitch) slot.
	for (uint32_t i = 0; i < 128 * 128; i++)
		cursorVram[i] = 0;
	for (uint32_t r = 0; r < 64; r++)
		for (uint32_t c = 0; c < 64; c++)
			cursorVram[r * 128 + c] = 0xFFFFFFFFu;

	// HUBP cursor control image in the DMUB union layout (enable, mode[10:8],
	// pitch[17:16], lines_per_chunk[28:24]; REQ_MODE is not ours to set here).
	const uint32_t hubpCtl = 1u | (hwCursorMode << 8) |
	                         (1u << 16) /*pitch 128px*/ | (3u << 24);
	const uint32_t dppCtl  = 1u | (hwCursorMode << 4);

	const uint32_t pipeIdx = pipe.hubp < Pipe::kMaxOtg ? pipe.hubp : 0;
	const uint32_t otgIdx  = pipe.otg < Pipe::kMaxOtg ? pipe.otg : 0;
	Dmub::Cmd cmds[2] = {
		{
			Dmub::headerWord(Dmub::CmdUpdateCursorInfo, 0, 52, false, /*multi=*/true),
			100, 100, 64, 64,          // cursor_rect x,y,w,h
			0,                         // debug flags
			0x00020001u | (pipeIdx << 8),   // enable=1, pipe, VERSION_2 (external
			                           // monitor support — v0 makes the FW park
			                           // the cursor, observed on hardware), panel 0
			hubpCtl,
			100u | (100u << 16),       // position: x [15:0], y [31:16] per DMUB
			0,                         // hot spot
			0,                         // dst offset
			dppCtl,
			pipeIdx,                   // position pipe_idx + padding
			otgIdx,                    // otg_inst + padding
			0, 0,
		},
		{
			Dmub::headerWord(Dmub::CmdUpdateCursorInfo, 0, 24),
			static_cast<uint32_t>(cursorMcAddr >> 32) & 0xffff,   // SURFACE_ADDR_HIGH
			static_cast<uint32_t>(cursorMcAddr),                  // SURFACE_ADDR
			hubpCtl,
			64u | (64u << 16),         // size: width, height
			3u << 8,                   // settings: chunk_hdl_adjust
			dppCtl,
			0, 0, 0, 0, 0, 0, 0, 0, 0,
		},
	};

	bool consumed = dmubSubmit(cmds, 2, "dmub-cursor");
	FBLOG("dmub: cursor-info %s; look for a white 64x64 square at (100,100); "
	      "hubp ctl rb=0x%08x pos rb=0x%08x cm rb=0x%08x",
	      consumed ? "CONSUMED" : "NOT consumed",
	      regReadDmu(2, 0x0679 + hubpOff()), regReadDmu(2, 0x067d + hubpOff()),
	      regReadDmu(2, 0x0cf1 + dppOff()));
}


#endif

// ---------------------------------------------------------------------------
// NDRV hardware cursor (W14, W21)
// ---------------------------------------------------------------------------

#include "device.hpp"
#include "ndrv.hpp"

#include <stdarg.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <IOKit/ndrvsupport/IOMacOSVideo.h>

#define FBLOG(fmt, ...) IOLog("RDNA4FB: " fmt "\n", ## __VA_ARGS__)

namespace {

// Linux dcn_4_1_0_offset.h: regCURSOR0_0_* at lines 2334-2349,
// all BASE_IDX 2; regCM_CUR0_* at lines 3326-3337, BASE_IDX 2.
constexpr uint32_t kCursorControl = 0x0679;
constexpr uint32_t kCursorAddress = 0x067a;
constexpr uint32_t kCursorAddressHigh = 0x067b;
constexpr uint32_t kCursorSize = 0x067c;
constexpr uint32_t kCursorPosition = 0x067d;
constexpr uint32_t kCursorHotSpot = 0x067e;
constexpr uint32_t kCursorDstOffset = 0x0680;
constexpr uint32_t kCursorSettings = 0x0653; // regHUBPREQ0_CURSOR_SETTINGS
constexpr uint32_t kCursorCmControl = 0x0cf1;
constexpr uint32_t kCursorCmScaleGY = 0x0cf4;
constexpr uint32_t kCursorCmScaleRB = 0x0cf5;
constexpr uint32_t kCursorCmMatrix = 0x0cf6;
// Address routing (log only): regDCN_VM_FB_LOCATION_BASE/TOP 0x0475/0x0476,
// regDCN_VM_AGP_BOT/TOP/BASE 0x0478/0x0479/0x047a, per HUBP
// regHUBPREQ0_DCN_VM_SYSTEM_APERTURE_LOW/HIGH_ADDR 0x062c/0x062d and
// regHUBPREQ0_DCN_VM_MX_L1_TLB_CNTL 0x063a; OTG0 regs below are +otgOff().
constexpr uint32_t kVmFbBase = 0x0475, kVmFbTop = 0x0476;
constexpr uint32_t kVmAgpBot = 0x0478, kVmAgpTop = 0x0479, kVmAgpBase = 0x047a;
constexpr uint32_t kSysApLow = 0x062c, kSysApHigh = 0x062d, kL1TlbCntl = 0x063a;
constexpr uint32_t kOtgMasterUpdateLock = 0x1b89, kOtgDoubleBufferCtl = 0x1b5c;
constexpr uint32_t kOtgGlobalSyncStatus = 0x1b88;
// The other DPP-side double-buffered blocks and the MPC that gates their update
// (dcn_4_1_0_offset.h): CNVC_CFG0_FORMAT_CONTROL 0x0cd0 (CNVC_UPDATE_PENDING bit 20),
// CM0_CM_CONTROL 0x0d67 (CM_UPDATE_PENDING bit 8), DPP_TOP0_DPP_CONTROL 0x0cc5 (base 2,
// stride 0x16b per DPP); base 3: MPCC0_MPCC_CONTROL 0x0003, _UPDATE_LOCK_SEL 0x0005
// (SEL [3:0], LOCKED_STATUS [6:4]), _STATUS 0x000e (stride 0x15), MPC_OUT0_MUX 0x02f2
// (stride 4). If these pend or stay locked too, the whole DPP update domain is not
// latching, not just the cursor.
constexpr uint32_t kCnvcFormatControl = 0x0cd0, kCmControl = 0x0d67, kDppTopControl = 0x0cc5;
constexpr uint32_t kMpccControl = 0x0003, kMpccUpdateLockSel = 0x0005, kMpccStatus = 0x000e;
constexpr uint32_t kMpccStride = 0x15, kMpcOutMux = 0x02f2, kMpcOutStride = 4;
// The MPC cursor lock and its neighbours (dcn_4_1_0_offset.h:5455-5467, base 3, opp 0): five
// lock-set registers per OPP in this order (stride 5): ADR_CFG_CUR, ADR_CFG, ADR, CFG,
// CUR_VUPDATE_LOCK_SET0 (0x02c5, bit 0). DC writes only the last one (mpc1_cursor_lock,
// dcn10_mpc.c:458-463), around every cursor update. MPC_DPP_PENDING_STATUS 0x02bf and
// MPC_PENDING_STATUS_MISC 0x02c0 show which blocks still have updates pending.
constexpr uint32_t kMpcDppPending = 0x02bf, kMpcPendingMisc = 0x02c0;
constexpr uint32_t kMpcLockBase = 0x02c1, kMpcLockStride = 5, kMpcCurLock = 4;

// The cursor surface is one fixed 64x64 slot, the way amdgpu's DM hands the
// hardware a whole cursor buffer (attr->width/height = the buffer, the image
// sits in its top-left corner over transparent pixels). The size, the pitch and
// LINES_PER_CHUNK (hubp2_get_lines_per_chunk: 33..64 px wide = 8 lines) must
// all describe that slot, not the image inside it.
constexpr uint32_t kCursorWidth = 64;
constexpr uint32_t kCursorHeight = 64;
constexpr uint32_t kCursorPitch = 64;
constexpr uint32_t kCursorBytes = kCursorWidth * kCursorHeight * sizeof(uint32_t);
constexpr uint32_t kCursorModePremultipliedArgb = 2; // dc_cursor_color_format
constexpr uint32_t kCursorFp16One = 0x3c00;
constexpr uint32_t kCursorReqModePrefetch = 1u << 2;
constexpr uint32_t kCursorModeShift = 8;
constexpr uint32_t kCursorPitchShift = 16;   // CURSOR_PITCH: 0 = 64 px
constexpr uint32_t kCursorLinesPerChunkShift = 24;
constexpr uint32_t kCursorLinesPer8 = 3;     // enum cursor_lines_per_chunk: 1,2,4,8,16 = 0..4
constexpr uint32_t kCursorCmModeShift = 4;
constexpr uint32_t kCursorCmWorkingBits = (1u << 7) | (1u << 2);
constexpr uint32_t kDchubGlobalTimerCntl = 0x0527;   // regDCHUBBUB_GLOBAL_TIMER_CNTL (REFDIV [3:0], ENABLE bit 12)

// W32: the cursor request scheduling block (dcn_4_1_0_offset.h, all base idx 2, HUBP0 values, + hubpOff()):
// HUBPREQ0_DCN_TTU_QOS_WM 0x0621, _GLOBAL_TTU_CNTL 0x0622, _SURF0_TTU_CNTL0/1 0x0623/0x0624,
// _SURF1_TTU_CNTL0 0x0625, _CUR0_TTU_CNTL0/1 0x0627/0x0628; HUBP0_HUBPREQ_DEBUG_DB 0x05fc, _HUBP_CLK_CNTL 0x05f5;
// CURSOR0_0_CURSOR_STEREO_CONTROL 0x067f, _MEM_PWR_CTRL/STATUS 0x0681/0x0682; MPCC0_MPCC_TOP_SEL/BOT_SEL/OPP_ID
// 0x0000/0x0001/0x0002 (base idx 3, stride kMpccStride). CUR0_TTU_CNTL0: REFCYC_PER_REQ_DELIVERY [22:0],
// QoS_LEVEL_FIXED [27:24], QoS_RAMP_DISABLE [28] (sh_mask:7550-7555); CNTL1: REFCYC_PER_REQ_DELIVERY_PRE [22:0].
constexpr uint32_t kTtuQosWm = 0x0621, kTtuGlobal = 0x0622, kTtuSurf0Cntl0 = 0x0623, kTtuSurf0Cntl1 = 0x0624;
constexpr uint32_t kTtuSurf1Cntl0 = 0x0625, kTtuCur0Cntl0 = 0x0627, kTtuCur0Cntl1 = 0x0628;
constexpr uint32_t kHubpreqDebugDb = 0x05fc, kHubpClkCntl = 0x05f5;
constexpr uint32_t kCursorStereo = 0x067f, kCursorMemPwrCtrl = 0x0681, kCursorMemPwrStatus = 0x0682;
constexpr uint32_t kMpccTopSel = 0x0000, kMpccBotSel = 0x0001, kMpccOppId = 0x0002;
constexpr uint32_t kTtuDeliveryMask = 0x007fffff;
constexpr uint32_t kMissionMode = 1u << 8;        // HUBPREQ_DEBUG_DB value amdgpu writes (dcn401_hubp.c:329)
constexpr uint32_t kTtuMaxStreamKHz = 600000;     // above this ODM combine halves the per-DPP clock
constexpr uint32_t kTtuQosFixedCur0 = 8;   // dml_display_rq_dlg_calc.c:497 qos_level_fixed_cur0 = 8, ramp not disabled (:500)
// W38 (dcn_4_1_0_offset.h, base idx 2 unless noted; DPP0/HUBP0/OTG0 values, + dppOff()/hubpOff()/otgOff()):
constexpr uint32_t kDsclSclMode = 0x0d08, kDsclControl = 0x0d0a, kDsclUpdate = 0x0d18, kDsclAutocal = 0x0d19;
constexpr uint32_t kDsclRecoutStart = 0x0d1e, kDsclRecoutSize = 0x0d1f, kDsclMpcSize = 0x0d20;
constexpr uint32_t kDsclLbFormat = 0x0d21, kDsclLbMemCtrl = 0x0d22, kDsclMemPwrCtrl = 0x0d24, kDsclMemPwrStatus = 0x0d25;
constexpr uint32_t kDsclObuf = 0x0d26, kDsclEasfH = 0x0d28, kDsclEasfV = 0x0d29, kDsclIsharp = 0x0d55;
constexpr uint32_t kDchubpCntl = 0x05f4, kMallConfig = 0x05f7, kExpansionMode = 0x0620, kHubpreqStatus2 = 0x0663;
constexpr uint32_t kOtgPipeUpdateStatus = 0x1b9e, kOtgFrameCount = 0x1b4d, kOtgGlobalCtrl2 = 0x1b90;
constexpr uint32_t kOtgCrcCntl = 0x1b65, kOtgCrcWindowAX = 0x1b66, kOtgCrcWindowAY = 0x1b67;
constexpr uint32_t kOtgCrcWindowBX = 0x1b68, kOtgCrcWindowBY = 0x1b69, kOtgCrcDataRg = 0x1b6a, kOtgCrcDataB = 0x1b6b;
// W40 (dcn_4_1_0_offset.h, base idx 2, DPP0 values, + dppOff(); DPP stride 0x16b = CM1_CM_CONTROL 0x0ed2 - 0x0d67):
constexpr uint32_t kCmPostCscControl = 0x0d68, kCmBiasCrR = 0x0d75, kCmBiasYgCbB = 0x0d76;
constexpr uint32_t kCmGamcorControl = 0x0d77, kCmHdrMult = 0x0dc1, kCmMemPwrCtrl = 0x0dc2, kCmMemPwrStatus = 0x0dc3;
constexpr uint32_t kCmDealpha = 0x0dc5;
constexpr uint32_t kHdrMultOne = 0x1f000;   // 1.0 in s6e12, CM_HDR_MULT_COEF [18:0] (dcn10_hwseq.c:3247)
// HUBPREQ0 DLG/TTU block written by hubp401_program_requestor / _program_dlg (dcn401_hubp.c:321-409), + hubpOff():
constexpr uint32_t kDlgFirst = 0x063b, kDlgLast = 0x0655;
constexpr uint32_t kCursorProbeMax = 10;   // trail budget: arming, self-test, up to three flips, first set/draw calls

uint32_t premultiplyArgb(uint32_t pixel) {
	const uint32_t alpha = pixel >> 24;
	if (alpha == 0xff)
		return pixel;
	if (alpha == 0)
		return 0;
	const uint32_t r = ((pixel >> 16) & 0xff) * alpha;
	const uint32_t g = ((pixel >> 8) & 0xff) * alpha;
	const uint32_t b = (pixel & 0xff) * alpha;
	return (alpha << 24) | (((r + 127) / 255) << 16) |
	       (((g + 127) / 255) << 8) | ((b + 127) / 255);
}

} // namespace

// Every cursor line goes to the kernel log AND to a registry property, because
// the macOS kernel message buffer wraps before the diagnostic batch reads it
// (boot 6 of round 2 lost every early line). The property is the durable copy:
// ioreg -l | grep RDNA4FB,Cursor. The trail is 8 KiB and append-only: it keeps
// EVERY cursor line and every "latch:" line until it is full (round 3 filled the
// old 2 KiB after 13 lines, so the later shown/hidden states were dropped);
// a full trail ends in "...(full)" so a cut-off is never mistaken for the end.
void RDNA4Device::cursorTrailAppend(const char *line) {
	// The compute thread's flip hook (cursorRegProbe) appends too; the lock exists from initHardwareCursor on.
	if (cursorTrailLock)
		IOLockLock(cursorTrailLock);
	cursorTrailAppendLocked(line);
	if (cursorTrailLock)
		IOLockUnlock(cursorTrailLock);
}

void RDNA4Device::cursorTrailAppendLocked(const char *line) {
	const size_t n = strlen(line);
	// One line in ioreg: the lines are joined with " ## " (diagnostic-log.sh splits them).
	const size_t reserve = 5 + sizeof("...(full)");
	if (cursorTrailFull || cursorTrailLen + n + reserve > sizeof(cursorTrail)) {
		if (!cursorTrailFull) {
			cursorTrailFull = true;
			strlcat(cursorTrail, "...(full)", sizeof(cursorTrail));
			if (owner)
				owner->setProperty("RDNA4FB,Cursor", cursorTrail);
		}
		return;
	}
	memcpy(cursorTrail + cursorTrailLen, line, n);
	cursorTrailLen += static_cast<uint16_t>(n);
	memcpy(cursorTrail + cursorTrailLen, " ## ", 4);
	cursorTrailLen += 4;
	cursorTrail[cursorTrailLen] = '\0';
	if (owner)
		owner->setProperty("RDNA4FB,Cursor", cursorTrail);
}

void RDNA4Device::cursorNote(const char *fmt, ...) {
	char line[288];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	IOLog("RDNA4FB: cursor: %s\n", line);
	cursorTrailAppend(line);
}

// The OTG update-latch lines (device.cpp ensureUpdateLatch) keep their own
// "RDNA4FB: latch:" log prefix; with the cursor requested they also go into the
// trail, where they explain why a cursor write stays pending.
void RDNA4Device::latchNote(const char *fmt, ...) {
	char line[288];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	IOLog("RDNA4FB: latch: %s\n", line);
	if (hwCursorRequested) {
		char tagged[300];
		snprintf(tagged, sizeof(tagged), "latch: %s", line);
		cursorTrailAppend(tagged);
	}
}

// Program the cursor plane's attributes (everything except the position) the
// way amdgpu's set_cursor_attributes does, enabled or not.
void RDNA4Device::cursorProgramPlane(bool enable) {
	ensureUpdateLatch();
	cursorMpcLock(true);   // dcn10_cursor_lock: the writes below latch together at the unlock
	const uint32_t hubp = hubpOff();
	const uint32_t dpp = dppOff();
	regWriteDmu(2, kCursorAddressHigh + hubp, static_cast<uint32_t>(cursorMcAddr >> 32) & 0xffff);
	regWriteDmu(2, kCursorAddress + hubp, static_cast<uint32_t>(cursorMcAddr));
	// hubp32_cursor_set_attributes: SIZE is the buffer, not the image inside it.
	regWriteDmu(2, kCursorSize + hubp, kCursorHeight | (kCursorWidth << 16));
	// The hardware hot spot stays 0: IOFramebuffer hands cscDrawHardwareCursor the
	// hot-spot-adjusted top-left corner (the parked cursor.cpp said so as well), and
	// the hot spot register subtracts a second time (hubp401_cursor_set_position).
	// drawHardwareCursor uses it only to represent a corner above/left of the screen.
	regWriteDmu(2, kCursorHotSpot + hubp, 0);
	regWriteDmu(2, kCursorSettings + hubp, 3u << 8); // CURSOR0_CHUNK_HDL_ADJUST=3, DST_Y_OFFSET=0
	if (cursorPipeFixesOn) {   // W38: REG_UPDATE(DCHUBP_MALL_CONFIG, USE_MALL_FOR_CURSOR, cursor_size > 16384) (dcn32_hubp.c:133,165)
		const uint32_t mall = regReadDmu(2, kMallConfig + hubp);
		if (mall != 0xffffffffu && (mall & 4u))
			regWriteDmu(2, kMallConfig + hubp, mall & ~4u);   // 64x64 ARGB is exactly 16384 bytes: not above the limit
	}
	cursorCtlBase = kCursorReqModePrefetch | (kCursorModePremultipliedArgb << kCursorModeShift) |
	                (0u << kCursorPitchShift) | (kCursorLinesPer8 << kCursorLinesPerChunkShift);
	regWriteDmu(2, kCursorControl + hubp, cursorCtlBase | (enable ? 1u : 0u));
	regWriteDmu(2, kCursorCmScaleGY + dpp, kCursorFp16One);
	regWriteDmu(2, kCursorCmScaleRB + dpp, kCursorFp16One);
	regWriteDmu(2, kCursorCmMatrix + dpp, 0);
	regWriteDmu(2, kCursorCmControl + dpp,
	            kCursorCmWorkingBits | (kCursorModePremultipliedArgb << kCursorCmModeShift) |
	            (enable ? 1u : 0u));
	hwCursorEnabledHw = enable;
	cursorMpcLock(false);
	if (!cursorLockDepth)   // outermost bracket closed: does the update latch?
		cursorWaitLatched(enable ? "attributes+enable" : "attributes", 40);
}

// CURSOR_DST_X_OFFSET (hubp401_cursor_set_position): x scaled from pixel-clock
// to DCHUB refclk time. The refclk is read back like dcn20_hubbub.c:560-582
// (DCHUBBUB_GLOBAL_TIMER_CNTL; DC asserts 40-60 MHz), else DC's ~50 MHz; the value
// and its source are logged at arming. No pixel clock (bootTiming) gives 0.
uint32_t RDNA4Device::cursorDstXOffset(uint32_t px) const {
	const uint32_t pixelKHz = bootTimingValid ? bootTiming.pixelClockKHz : 0;
	return pixelKHz ? static_cast<uint32_t>((static_cast<uint64_t>(px) * cursorRefClkKHz) / pixelKHz) : 0;
}

// W32 read-only dump (docs/cursor-ttu.md, analysis rank 1/2/3/4/5): the cursor's request scheduling next to the
// surface's (a CUR0 pair of 0 beside a programmed SURF0 pair is the rank-1 signature), the clock/memory power
// state of the HUBP and its cursor memory, which DPP/OPP MPCC0 really selects, and three sprite pixels read back
// from VRAM. Taken at arming, after the self-test programming, after every boot flip (compute thread) and at the
// first macOS cursor calls: the state seen at the screen, not only the state right after init.
void RDNA4Device::cursorRegProbe(const char *why) {
	if (!hwCursorReady || cursorProbeLogs >= kCursorProbeMax)
		return;
	cursorProbeLogs++;
	const uint32_t hubp = hubpOff();
	const uint32_t mpcc = pipe.hubp < Pipe::kMaxOtg ? pipe.hubp * kMpccStride : 0;
	cursorNote("%s: ttu: cur0 0x%08x/0x%08x surf0 0x%08x/0x%08x surf1 0x%08x global 0x%08x qos_wm 0x%08x "
	           "debug_db 0x%08x", why,
	           regReadDmu(2, kTtuCur0Cntl0 + hubp), regReadDmu(2, kTtuCur0Cntl1 + hubp),
	           regReadDmu(2, kTtuSurf0Cntl0 + hubp), regReadDmu(2, kTtuSurf0Cntl1 + hubp),
	           regReadDmu(2, kTtuSurf1Cntl0 + hubp), regReadDmu(2, kTtuGlobal + hubp),
	           regReadDmu(2, kTtuQosWm + hubp), regReadDmu(2, kHubpreqDebugDb + hubp));
	// Sprite pixels: first, centre, last of the 64x64 slot (the self-test fills 0xffff00ff).
	const uint32_t px0 = cursorVram ? cursorVram[0] : 0;
	const uint32_t pxMid = cursorVram ? cursorVram[(kCursorHeight / 2) * kCursorPitch + kCursorWidth / 2] : 0;
	const uint32_t pxLast = cursorVram ? cursorVram[kCursorHeight * kCursorPitch - 1] : 0;
	cursorNote("%s: gate: hubp clk 0x%08x cursor mem pwr 0x%08x/0x%08x stereo 0x%08x | mpcc%u top 0x%08x bot 0x%08x "
	           "opp 0x%08x | sprite px first/mid/last 0x%08x/0x%08x/0x%08x%s", why,
	           regReadDmu(2, kHubpClkCntl + hubp), regReadDmu(2, kCursorMemPwrCtrl + hubp),
	           regReadDmu(2, kCursorMemPwrStatus + hubp), regReadDmu(2, kCursorStereo + hubp),
	           pipe.hubp, regReadDmu(3, kMpccTopSel + mpcc), regReadDmu(3, kMpccBotSel + mpcc),
	           regReadDmu(3, kMpccOppId + mpcc), px0, pxMid, pxLast,
	           cursorHold ? (pxMid == 0xffff00ffu ? " (self-test data ok)" : " (SELF-TEST DATA GONE)") : "");
}

// W34 (W32 review S1), rdna4-cursor=2 only: the one DLG-side write amdgpu makes at the top of
// hubp401_program_deadline (dcn401_hubp.c:329) and the kext never did: REG_WRITE(HUBPREQ_DEBUG_DB, 1 << 8),
// "put DLG in mission mode". The whole register is written, not a bit (DC's REG_WRITE), and only when it does not
// already hold exactly that; the old value, the new one and a readback are logged. rdna4-cursordlg=0 skips it.
// W35 (W34 review S1): bit 8 is not cursor-only. amdgpu's own comments call it "disable dlg test mode" / "hack mode
// disable" (dcn20_hubp.c:176, dcn10_hubp.c:129): it takes the whole HUBP request generator, the primary surface
// included, from the DLG test mode to the mode that follows the programmed DLG/TTU registers, and DC writes it in
// the same call that programs those registers. So it is written only when the GOP already programmed the DLG
// (DCN_SURF0_TTU_CNTL0 delivery non-zero) and bit 8 is clear; otherwise the primary plane would start following
// registers nobody set (underflow / blank, misread as a cursor symptom) and the write is skipped and logged.
bool RDNA4Device::cursorProgramMissionMode() {
	uint32_t enable = 1;
	if (PE_parse_boot_argn("rdna4-cursordlg", &enable, sizeof(enable)) && enable == 0) {
		cursorNote("rdna4-cursordlg=0: HUBPREQ_DEBUG_DB left alone (control)");
		return false;
	}
	const uint32_t reg = kHubpreqDebugDb + hubpOff();
	const uint32_t old = regReadDmu(2, reg);
	if (old == 0xffffffffu) {
		cursorNote("HUBPREQ_DEBUG_DB unreadable (0x%08x): mission mode not written", old);
		return false;
	}
	if (old & kMissionMode) {
		cursorNote("HUBPREQ_DEBUG_DB is already 0x%08x (bit 8 set, DLG mission mode): nothing to write", old);
		return false;
	}
	const uint32_t surf0 = regReadDmu(2, kTtuSurf0Cntl0 + hubpOff()) & kTtuDeliveryMask;
	if (!surf0) {
		cursorNote("DLG registers not programmed by the GOP (SURF0_TTU_CNTL0 delivery 0, HUBPREQ_DEBUG_DB 0x%08x); "
		           "mission mode NOT written", old);
		return false;
	}
	regWriteDmu(2, reg, kMissionMode);
	cursorNote("HUBPREQ_DEBUG_DB was 0x%08x (SURF0 delivery 0x%x programmed by the GOP), wrote 0x%08x (DLG mission mode, "
	           "dcn401_hubp.c:329); reads back 0x%08x", old, surf0, kMissionMode, regReadDmu(2, reg));
	return true;
}

// W32, opt-in since W34 (rdna4-cursor=2 with rdna4-cursorttu=1, call inside the lock bracket). NOT what amdgpu does
// on DCN 4.01: DML 2.1 (using_dml21, dcn401_resource.c:784) never fills refcyc_per_req_delivery_cur0, so amdgpu
// writes DCN_CUR0_TTU_CNTL0/1 = 0 there and its cursor works (W32 review S1). The values below are the LEGACY DML2
// (dml2_0 core, DCN3.x-style) ones, an untested combination on this ASIC, kept as a secondary experiment.
// The registers are DCN_CUR0_TTU_CNTL0/1 (hubp401_program_deadline dcn401_hubp.c:406-409, _setup_interdependent
// :473-474). DML (display_mode_core.c:3441-3448, VRatio <= 1, HRatio 1) delivers one cursor request every
//   CursorRequestDeliveryTime [us] = width / pixel clock [MHz] / cursor_req_per_width,
//   cursor_req_per_width = ceil(width * 32 bpp / 256 / 8) = 1 for the 64 px slot;
// dml_display_rq_dlg_calc.c:403-404,486-487 turns it into DCHUB ref cycles times 2^10, with QoS_LEVEL_FIXED 8 and
// the ramp enabled (:497,:500); the prefetch pair (_PRE) is the same value while VRatioPrefetchY <= 1. The
// pixel clock is the boot timing's, the ref clock the DCHUB one read at arming. Without a pixel clock the value
// falls back to the surface's own SURF0 delivery value (the analysis' first cheap test). Only written with
// rdna4-cursorttu=1 (default off: amdgpu leaves the pair at 0).
bool RDNA4Device::cursorProgramTtu() {
	uint32_t enable = 0;
	if (!PE_parse_boot_argn("rdna4-cursorttu", &enable, sizeof(enable)) || enable != 1) {
		cursorNote("DCN_CUR0_TTU_CNTL0/1 left alone (rdna4-cursorttu=1 writes DML-style values; amdgpu on DCN 4.01 leaves them 0)");
		return false;
	}
	const uint32_t hubp = hubpOff();
	const uint32_t pixelKHz = bootTimingValid ? bootTiming.pixelClockKHz : 0;
	if (pixelKHz > kTtuMaxStreamKHz) {   // ODM combine feeds each DPP half the stream clock: the formula would be off by 2
		cursorNote("CUR0 TTU not programmed: stream clock %u kHz is above %u kHz (ODM combine, the DML value would be doubled)",
		           pixelKHz, kTtuMaxStreamKHz);
		return false;
	}
	uint32_t delivery = 0;
	const char *source = "";
	if (pixelKHz) {
		const uint64_t reqPerWidth = (static_cast<uint64_t>(kCursorWidth) * 32 + 2047) / 2048;
		delivery = static_cast<uint32_t>((static_cast<uint64_t>(kCursorWidth) * cursorRefClkKHz * 1024) /
		                                 (static_cast<uint64_t>(pixelKHz) * reqPerWidth));
		source = "DML: width / pixel clock / req_per_width * ref clock * 2^10";
	} else {
		delivery = regReadDmu(2, kTtuSurf0Cntl0 + hubp) & kTtuDeliveryMask;
		source = "no pixel clock: SURF0_TTU_CNTL0's delivery value copied";
	}
	if (!delivery || delivery >= kTtuDeliveryMask) {
		cursorNote("CUR0 TTU not programmed: no usable delivery value (0x%x from %s)", delivery, source);
		return false;
	}
	const uint32_t cntl0 = delivery | (kTtuQosFixedCur0 << 24);
	regWriteDmu(2, kTtuCur0Cntl0 + hubp, cntl0);
	regWriteDmu(2, kTtuCur0Cntl1 + hubp, delivery);
	cursorNote("CUR0 TTU (legacy DML2, opt-in): CNTL0 0x%08x CNTL1 0x%08x (pixel %u kHz, ref %u kHz); reads back 0x%08x/0x%08x",
	           cntl0, delivery, pixelKHz, cursorRefClkKHz, regReadDmu(2, kTtuCur0Cntl0 + hubp),
	           regReadDmu(2, kTtuCur0Cntl1 + hubp));
	return true;
}


// ---------------------------------------------------------------------------
// W38 (premetal/rootcause-cursor.md), rdna4-cursor=2 only: DSCL/pipe state dump, the pipe-level fields amdgpu writes
// and we never did, the DSCL scaler-bypass set (opt-in) and the OTG CRC A/B that lets the card say by itself whether
// the cursor pixels reach the output.
// ---------------------------------------------------------------------------

// Read-only. dcn_4_1_0_offset.h, base idx 2, DPP0/HUBP0 values (+ dppOff()/hubpOff()): DSCL0_SCL_MODE 0x0d08,
// _RECOUT_START/SIZE 0x0d1e/0x0d1f, _MPC_SIZE 0x0d20, _LB_DATA_FORMAT/_LB_MEMORY_CTRL 0x0d21/0x0d22,
// _DSCL_MEM_PWR_CTRL/STATUS 0x0d24/0x0d25, _OBUF_CONTROL 0x0d26, _DSCL_UPDATE 0x0d18, _DSCL_AUTOCAL 0x0d19,
// _DSCL_CONTROL 0x0d0a, _DSCL_EASF_H/V_MODE 0x0d28/0x0d29, _ISHARP_MODE 0x0d55; HUBP0_DCHUBP_CNTL 0x05f4,
// _DCHUBP_MALL_CONFIG 0x05f7; HUBPREQ0_DCN_EXPANSION_MODE 0x0620, _HUBPREQ_STATUS_REG2 0x0663;
// OTG0_OTG_PIPE_UPDATE_STATUS 0x1b9e. MPCC0_MPCC_UPDATE_LOCK_SEL is MPC (base idx 3) 0x0005 (kMpccUpdateLockSel).
void RDNA4Device::cursorDsclDump(const char *why) {
	const uint32_t dpp = dppOff(), hubp = hubpOff(), otg = otgOff();
	const uint32_t mpcc = pipe.hubp < Pipe::kMaxOtg ? pipe.hubp * kMpccStride : 0;
	const uint32_t mode = regReadDmu(2, kDsclSclMode + dpp);
	cursorNote("%s: dscl: scl_mode 0x%08x (DSCL_MODE %u) recout 0x%08x/0x%08x mpc_size 0x%08x lb 0x%08x/0x%08x "
	           "mem_pwr 0x%08x/0x%08x obuf 0x%08x update 0x%08x autocal 0x%08x ctl 0x%08x easf h/v 0x%08x/0x%08x "
	           "isharp 0x%08x", why, mode, mode & 7, regReadDmu(2, kDsclRecoutStart + dpp),
	           regReadDmu(2, kDsclRecoutSize + dpp), regReadDmu(2, kDsclMpcSize + dpp),
	           regReadDmu(2, kDsclLbFormat + dpp), regReadDmu(2, kDsclLbMemCtrl + dpp),
	           regReadDmu(2, kDsclMemPwrCtrl + dpp), regReadDmu(2, kDsclMemPwrStatus + dpp),
	           regReadDmu(2, kDsclObuf + dpp), regReadDmu(2, kDsclUpdate + dpp), regReadDmu(2, kDsclAutocal + dpp),
	           regReadDmu(2, kDsclControl + dpp), regReadDmu(2, kDsclEasfH + dpp), regReadDmu(2, kDsclEasfV + dpp),
	           regReadDmu(2, kDsclIsharp + dpp));
	const uint32_t mall = regReadDmu(2, kMallConfig + hubp), exp = regReadDmu(2, kExpansionMode + hubp);
	cursorNote("%s: pipe: dchubp_cntl 0x%08x mall_cfg 0x%08x (USE_MALL_FOR_CURSOR %u) expansion 0x%08x (CRQ %u) "
	           "cursor mem pwr 0x%08x/0x%08x mpcc%u lock_sel 0x%08x | otg pipe_update_status 0x%08x "
	           "hubpreq_status2 0x%08x", why, regReadDmu(2, kDchubpCntl + hubp), mall, (mall >> 2) & 1, exp,
	           (exp >> 2) & 3, regReadDmu(2, kCursorMemPwrCtrl + hubp), regReadDmu(2, kCursorMemPwrStatus + hubp),
	           pipe.hubp, regReadDmu(3, kMpccUpdateLockSel + mpcc), regReadDmu(2, kOtgPipeUpdateStatus + otg),
	           regReadDmu(2, kHubpreqStatus2 + hubp));
}

// W40 (hub-task-273), read-only: every static register of the display pipe compared with what Linux amdgpu 7.2.2 leaves on
// this card with a visible cursor (E:\linux\rdna4-groundtruth-vkcube-20260929-232026, identical to the idle capture apart
// from addresses, positions and counters, which are not in the table). Only the differences are logged, as
// NAME=got/want (both masked), so the whole comparison costs a few lines of the trail.
struct LinuxRef { const char *name; uint8_t kind, seg; uint16_t off; uint32_t mask, want; };
static const LinuxRef kLinuxRef[] = {
#include "cursor_linux_ref.inc"
};

void RDNA4Device::cursorLinuxDiff(const char *why) {
	const uint32_t hubp = hubpOff(), dpp = dppOff();
	const uint32_t mpcc = pipe.hubp < Pipe::kMaxOtg ? pipe.hubp * kMpccStride : 0;
	char line[250];
	size_t n = 0;
	uint32_t total = 0, diff = 0, shown = 0, dead = 0, part = 0;
	line[0] = '\0';
	for (const LinuxRef &r : kLinuxRef) {
		total++;
		const uint32_t delta = r.kind == 1 ? hubp : r.kind == 2 ? dpp : r.kind == 3 ? mpcc : 0;
		const uint32_t v = regReadDmu(r.seg, r.off + delta);
		if (v == 0xffffffffu) {
			dead++;
			continue;
		}
		if (!((v ^ r.want) & r.mask))
			continue;
		diff++;
		if (shown >= 70)
			continue;
		char item[56];
		const size_t m = static_cast<size_t>(snprintf(item, sizeof(item), " %s=%x/%x", r.name, v & r.mask, r.want));
		if (n + m >= sizeof(line)) {
			cursorNote("linuxdiff %s #%u:%s", why, ++part, line);
			n = 0;
			line[0] = '\0';
		}
		memcpy(line + n, item, m + 1);
		n += m;
		shown++;
	}
	if (n)
		cursorNote("linuxdiff %s #%u:%s", why, ++part, line);
	cursorNote("linuxdiff %s: %u of %u registers differ from the Linux capture (%u shown as NAME=got/want, %u unreadable)",
	           why, diff, total, shown, dead);
}

// W40, read-only: the DPP colour-management block the GOP left (CM0_CM_CONTROL.CM_BYPASS is the candidate,
// premetal/verify-cursor.md section 2) and the DLG/TTU registers 0x063b-0x0655 (HUBPREQ0_BLANK_OFFSET_0 ..
// _DST_Y_DELTA_DRQ_LIMIT), nine per line to stay inside the 288-byte note.
void RDNA4Device::cursorCmDump(const char *why) {
	const uint32_t dpp = dppOff();
	const uint32_t ctl = regReadDmu(2, kCmControl + dpp);
	cursorNote("%s: cm: control 0x%08x (CM_BYPASS %u, UPDATE_PENDING %u) post_csc 0x%08x bias 0x%08x/0x%08x gamcor 0x%08x "
	           "hdr_mult 0x%08x mem_pwr 0x%08x/0x%08x dealpha 0x%08x", why, ctl, ctl & 1, (ctl >> 8) & 1,
	           regReadDmu(2, kCmPostCscControl + dpp), regReadDmu(2, kCmBiasCrR + dpp), regReadDmu(2, kCmBiasYgCbB + dpp),
	           regReadDmu(2, kCmGamcorControl + dpp), regReadDmu(2, kCmHdrMult + dpp), regReadDmu(2, kCmMemPwrCtrl + dpp),
	           regReadDmu(2, kCmMemPwrStatus + dpp), regReadDmu(2, kCmDealpha + dpp));
}

void RDNA4Device::cursorDlgDump(const char *why) {
	const uint32_t hubp = hubpOff();
	for (uint32_t base = kDlgFirst; base <= kDlgLast; base += 9) {
		char line[200];
		size_t n = static_cast<size_t>(snprintf(line, sizeof(line), "%s: dlg 0x%04x:", why, base));
		for (uint32_t r = base; r < base + 9 && r <= kDlgLast && n < sizeof(line) - 12; r++)
			n += static_cast<size_t>(snprintf(line + n, sizeof(line) - n, " %08x", regReadDmu(2, r + hubp)));
		cursorNote("%s", line);
	}
}

// The pipe-level cursor fields amdgpu writes (rdna4-cursorpipe=0 skips them: the control), each with its source:
//  - rdna4-cursorcrq=1: CRQ_EXPANSION_MODE = 1. hubp401_program_requestor writes DCN_EXPANSION_MODE (DRQ/PRQ/CRQ/MRQ)
//    from DML for every plane (dcn401_hubp.c:293-305, dml2_core_dcn4_calcs.c:12500-12509): it is a SURFACE requestor
//    field of the live primary pipe, not a cursor field (W38 review S2), so it is opt-in and only the CRQ field
//    [3:2] is touched (amdgpu writes all four). By default it is only logged.
//  - the cursor return buffer memory (CURSOR_MEM_PWR_CTRL/STATUS 0x0681/0x0682): DC never writes it, so on Linux it
//    is at its reset value; CROB_MEM_PWR_FORCE [1:0] is cleared only if it is set (a non-zero STATUS alone is a
//    normal light sleep) and the status read again.
//  - rdna4-cursormpcsel=1: MPCC_UPDATE_LOCK_SEL = OPP number, "Configure VUPDATE lock set for this MPCC to map to
//    the OPP" (dcn10_mpc.c:221-222). The GOP left 0xf. Mapping it puts the MPCC's updates under the OTG master
//    update lock, which changes when everything on this MPCC latches, so it has its own control.
// USE_MALL_FOR_CURSOR is written with every attribute set (cursorProgramPlane), like dcn32_hubp.c:133,165.
void RDNA4Device::cursorPipeFixes() {
	uint32_t enable = 1;
	if (PE_parse_boot_argn("rdna4-cursorpipe", &enable, sizeof(enable)) && enable == 0) {
		cursorPipeFixesOn = false;
		cursorNote("rdna4-cursorpipe=0: USE_MALL_FOR_CURSOR / cursor memory power left alone (control)");
		return;
	}
	cursorPipeFixesOn = true;
	const uint32_t hubp = hubpOff();
	const uint32_t exp = regReadDmu(2, kExpansionMode + hubp);
	uint32_t crq = 0;
	if (!(PE_parse_boot_argn("rdna4-cursorcrq", &crq, sizeof(crq)) && crq == 1)) {
		cursorNote("DCN_EXPANSION_MODE 0x%08x (CRQ %u) left alone: a surface-requestor field of the primary pipe "
		           "(rdna4-cursorcrq=1 writes CRQ = 1)", exp, (exp >> 2) & 3);
	} else if (exp != 0xffffffffu && ((exp >> 2) & 3) != 1) {
		regWriteDmu(2, kExpansionMode + hubp, (exp & ~0xcu) | (1u << 2));
		cursorNote("DCN_EXPANSION_MODE was 0x%08x (CRQ %u), CRQ_EXPANSION_MODE set to 1 (dcn401_hubp.c:293-305); reads back 0x%08x",
		           exp, (exp >> 2) & 3, regReadDmu(2, kExpansionMode + hubp));
	} else {
		cursorNote("DCN_EXPANSION_MODE is already 0x%08x (CRQ %u): nothing to write", exp, (exp >> 2) & 3);
	}
	const uint32_t status = regReadDmu(2, kCursorMemPwrStatus + hubp);
	const uint32_t ctrl = regReadDmu(2, kCursorMemPwrCtrl + hubp);
	if (status != 0xffffffffu && (status & 3) && (ctrl & 3)) {
		regWriteDmu(2, kCursorMemPwrCtrl + hubp, ctrl & ~3u);
		uint32_t waited = 0, now = regReadDmu(2, kCursorMemPwrStatus + hubp);
		while ((now & 3) && waited < 5) {
			IOSleep(1);
			waited++;
			now = regReadDmu(2, kCursorMemPwrStatus + hubp);
		}
		cursorNote("cursor memory power: STATUS was 0x%08x (CTRL 0x%08x), CROB_MEM_PWR_FORCE cleared; CTRL now 0x%08x "
		           "STATUS 0x%08x after %u ms", status, ctrl, regReadDmu(2, kCursorMemPwrCtrl + hubp), now, waited);
	} else {
		cursorNote("cursor memory power: STATUS 0x%08x, CTRL 0x%08x: CROB_MEM_PWR_FORCE already 0, nothing to write",
		           status, ctrl);
	}
	uint32_t sel = 0;
	if (PE_parse_boot_argn("rdna4-cursormpcsel", &sel, sizeof(sel)) && sel == 1) {
		const uint32_t mpcc = pipe.hubp < Pipe::kMaxOtg ? pipe.hubp * kMpccStride : 0;
		const uint32_t opp = pipe.opp < Pipe::kMaxOtg ? pipe.opp : 0;
		const uint32_t old = regReadDmu(3, kMpccUpdateLockSel + mpcc);
		regWriteDmu(3, kMpccUpdateLockSel + mpcc, opp);
		cursorNote("MPCC%u_MPCC_UPDATE_LOCK_SEL was 0x%08x, set to OPP %u (dcn10_mpc.c:221-222); reads back 0x%08x",
		           pipe.hubp, old, opp, regReadDmu(3, kMpccUpdateLockSel + mpcc));
	} else {
		cursorNote("MPCC_UPDATE_LOCK_SEL left alone (rdna4-cursormpcsel=1 maps it to the OPP as amdgpu does)");
	}
}

// rdna4-cursordscl=1 (with rdna4-cursor=2): ONLY if the DSCL is in full bypass (DSCL_MODE 6), program the mode-0
// (SCALING_444_BYPASS) set amdgpu writes for a 1:1 RGB plane (dpp401_dscl_set_scaler_manual_scale,
// dcn401_dpp_dscl.c:1067-1161; DCN401 never selects DSCL_BYPASS for RGB, :120-133, dc_spl.c:784-791), under the OTG
// update lock like modeset.cpp:292-300. A scaling setup (modes 1-5, RECOUT/MPC_SIZE different from the plane) is a
// legitimate GOP configuration and is never overwritten (W38 review S3); mode 0 with an unset RECOUT is only
// reported. The GOP's values are logged first so they can be restored.
void RDNA4Device::cursorDsclDecide() {
	const uint32_t dpp = dppOff(), otg = otgOff();
	uint32_t want = 0;
	const bool asked = PE_parse_boot_argn("rdna4-cursordscl", &want, sizeof(want)) && want == 1;
	const uint32_t mode = regReadDmu(2, kDsclSclMode + dpp) & 7;
	const uint32_t recout = regReadDmu(2, kDsclRecoutSize + dpp), mpc = regReadDmu(2, kDsclMpcSize + dpp);
	const uint32_t w = fbWidth, h = fbHeight, size = w | (h << 16);
	const bool bypass = mode == 6, wrong = recout != size || mpc != size;
	const char *kase;
	if (bypass)
		kase = "case A: full bypass, the #1 candidate is LIVE";
	else if (mode == 0 && wrong)
		kase = "case B: mode 0 with RECOUT/MPC_SIZE unset or not the plane size, the #1 candidate may be live, never written";
	else if (mode != 0)
		kase = "case C: a scaling mode, a legitimate GOP setup, never overwritten";
	else
		kase = "case D: mode 0 with RECOUT/MPC_SIZE equal to the plane, the #1 candidate is eliminated";
	if (!asked) {
		cursorNote("DSCL decision: DSCL_MODE %u, RECOUT 0x%08x, MPC_SIZE 0x%08x, plane 0x%08x -> %s; not written "
		           "(rdna4-cursordscl=1 programs amdgpu's mode-0 set in case A only)", mode, recout, mpc, size, kase);
		return;
	}
	if (!bypass) {
		cursorNote("DSCL decision: DSCL_MODE %u, RECOUT 0x%08x, MPC_SIZE 0x%08x, plane 0x%08x -> %s; not written",
		           mode, recout, mpc, size, kase);
		return;
	}
	if (!w || !h) {
		cursorNote("DSCL decision: no plane size known (fb %ux%u): not written", w, h);
		return;
	}
	cursorNote("DSCL decision: DSCL_MODE %u, RECOUT 0x%08x, MPC_SIZE 0x%08x -> %s; programming amdgpu's mode-0 set for a "
	           "%ux%u plane (GOP values above, restore with them if the screen breaks)", mode, recout, mpc, kase, w, h);
	// dcn401_program_pipe: OTG update lock, wait for it to be held (modeset.cpp:292-294).
	regWriteDmu(2, kOtgGlobalCtrl2 + otg, (regReadDmu(2, kOtgGlobalCtrl2 + otg) & ~(0x7u << 25)) |
	                                      (static_cast<uint32_t>(pipe.otg) << 25));
	regWriteDmu(2, kOtgMasterUpdateLock + otg, regReadDmu(2, kOtgMasterUpdateLock + otg) | 1u);
	uint32_t waited = 0;
	while (!(regReadDmu(2, kOtgMasterUpdateLock + otg) & (1u << 8)) && waited < 10) {
		IOSleep(1);
		waited++;
	}
	if (!(regReadDmu(2, kOtgMasterUpdateLock + otg) & (1u << 8))) {
		regWriteDmu(2, kOtgMasterUpdateLock + otg, regReadDmu(2, kOtgMasterUpdateLock + otg) & ~1u);
		cursorNote("DSCL write: the OTG update lock was not held after 10 ms; nothing written");
		return;
	}
	// dpp401_power_on_dscl (dcn401_dpp_dscl.c:149-163): LUT memory out of force before the DSCL is used.
	const uint32_t mp = regReadDmu(2, kDsclMemPwrCtrl + dpp);
	if (regReadDmu(2, kDsclMemPwrStatus + dpp) & 3) {
		regWriteDmu(2, kDsclMemPwrCtrl + dpp, mp & ~3u);
		for (uint32_t ms = 0; ms < 5 && (regReadDmu(2, kDsclMemPwrStatus + dpp) & 3); ms++)
			IOSleep(1);
	}
	regWriteDmu(2, kDsclAutocal + dpp, 0);                    // AUTOCAL_MODE off
	regWriteDmu(2, kDsclControl + dpp, 0);                    // SCL_BOUNDARY_MODE 0
	regWriteDmu(2, kDsclRecoutStart + dpp, 0);
	regWriteDmu(2, kDsclRecoutSize + dpp, size);
	regWriteDmu(2, kDsclMpcSize + dpp, size);
	regWriteDmu(2, kDsclSclMode + dpp, regReadDmu(2, kDsclSclMode + dpp) & ~7u);   // DSCL_MODE = 0 (444 bypass)
	regWriteDmu(2, kDsclLbFormat + dpp, 0);                   // INTERLEAVE_EN 0, ALPHA_EN 0
	regWriteDmu(2, kDsclLbMemCtrl + dpp, 63u << 8);           // MEMORY_CONFIG 0, LB_MAX_PARTITIONS 63
	regWriteDmu(2, kDsclEasfH + dpp, regReadDmu(2, kDsclEasfH + dpp) & ~1u);       // prefer_easf: EASF off at 1:1
	regWriteDmu(2, kDsclEasfV + dpp, regReadDmu(2, kDsclEasfV + dpp) & ~1u);
	regWriteDmu(2, kDsclIsharp + dpp, regReadDmu(2, kDsclIsharp + dpp) & ~1u);     // ISHARP off
	regWriteDmu(2, kOtgMasterUpdateLock + otg, regReadDmu(2, kOtgMasterUpdateLock + otg) & ~1u);
	uint32_t pend = 0;
	for (; pend < 50 && (regReadDmu(2, kDsclUpdate + dpp) & 1); pend++)
		IOSleep(1);
	cursorNote("DSCL mode-0 set written; DSCL_UPDATE pending cleared after %u ms (%s)", pend,
	           (regReadDmu(2, kDsclUpdate + dpp) & 1) ? "STILL PENDING" : "latched");
	cursorDsclDump("after the DSCL write");
}

// The OTG master update lock bracket the modeset path uses (modeset.cpp:292-300, dcn401_program_pipe): lock select,
// lock, wait for it to be held (<= 10 ms). Returns false, with the lock released, if it was not held.
bool RDNA4Device::cursorOtgUpdateLock(bool lock) {
	const uint32_t otg = otgOff();
	if (!lock) {
		regWriteDmu(2, kOtgMasterUpdateLock + otg, regReadDmu(2, kOtgMasterUpdateLock + otg) & ~1u);
		return true;
	}
	regWriteDmu(2, kOtgGlobalCtrl2 + otg, (regReadDmu(2, kOtgGlobalCtrl2 + otg) & ~(0x7u << 25)) |
	                                      (static_cast<uint32_t>(pipe.otg) << 25));
	regWriteDmu(2, kOtgMasterUpdateLock + otg, regReadDmu(2, kOtgMasterUpdateLock + otg) | 1u);
	for (uint32_t waited = 0; waited < 10; waited++) {
		if (regReadDmu(2, kOtgMasterUpdateLock + otg) & (1u << 8))
			return true;
		IOSleep(1);
	}
	if (regReadDmu(2, kOtgMasterUpdateLock + otg) & (1u << 8))
		return true;
	regWriteDmu(2, kOtgMasterUpdateLock + otg, regReadDmu(2, kOtgMasterUpdateLock + otg) & ~1u);
	return false;
}

// rdna4-cursorcm=1 (with rdna4-cursor=2): DCN4 put the cursor unit inside the DPP colour-management block, and the
// GOP leaves CM0_CM_CONTROL.CM_BYPASS = 1 where amdgpu always clears it when a plane is enabled
// (premetal/verify-cursor.md section 2). Program the CM the way amdgpu leaves it for an SDR RGB plane without a
// degamma, under the OTG update lock; the GOP values were logged first by cursorCmDump("selftest pre"):
//   CM_POST_CSC_CONTROL = 0   POST_CSC_MODE bypass       (dpp3_program_post_csc, dcn30_dpp.c:118-120; RGB selects BYPASS)
//   CM_BIAS_CR_R = CM_BIAS_Y_G_CB_B = 0                  (dpp3_program_cm_bias, dcn30_dpp_cm.c:160-170)
//   CM_DEALPHA = 0            dealpha off                (dpp3_program_cm_dealpha, dcn30_dpp_cm.c:149-158)
//   CM_GAMCOR_CONTROL = 0     GAMCOR_MODE bypass, whole-register REG_SET (dpp3_program_gamcor_lut, dcn30_dpp_cm.c:229-230)
//   CM_HDR_MULT_COEF [18:0] = 0x1f000 (1.0, s6e12)       (dpp3_set_hdr_multiplier, dcn30_dpp_cm.c:308-314; dcn10_hwseq.c:3247)
//   CM_CONTROL.CM_BYPASS = 0, LAST                       (dpp3_enable_cm_block, dcn30_dpp_cm.c:43-54, reached from
//                                                         dpp3_program_gamcor_lut, dcn401_dpp.c:223)
// then CM_UPDATE_PENDING (CM_CONTROL bit 8) is polled clear (<= 50 ms). Returns whether anything was written and latched.
bool RDNA4Device::cursorCmApply() {
	const uint32_t dpp = dppOff();
	const uint32_t ctl = regReadDmu(2, kCmControl + dpp);
	if (ctl == 0xffffffffu) {
		cursorNote("CM write: CM0_CM_CONTROL reads all ones (the block is not powered): nothing written");
		return false;
	}
	const bool identity = !(ctl & 1) && !(regReadDmu(2, kCmPostCscControl + dpp) & 3) &&
	                      !regReadDmu(2, kCmBiasCrR + dpp) && !regReadDmu(2, kCmBiasYgCbB + dpp) &&
	                      !(regReadDmu(2, kCmDealpha + dpp) & 3) && !(regReadDmu(2, kCmGamcorControl + dpp) & 3) &&
	                      (regReadDmu(2, kCmHdrMult + dpp) & 0x7ffff) == kHdrMultOne;
	if (identity) {
		cursorNote("CM write: CM_BYPASS is 0 and the sub-blocks are already the identity amdgpu leaves: nothing to write");
		return false;
	}
	cursorNote("CM write: CM_BYPASS was %u; programming amdgpu's SDR RGB CM state under the OTG update lock "
	           "(GOP values in the preceding 'cm:' line, restore with them if the picture breaks)", ctl & 1);
	if (!cursorOtgUpdateLock(true)) {
		cursorNote("CM write: the OTG update lock was not held after 10 ms; nothing written");
		return false;
	}
	regWriteDmu(2, kCmPostCscControl + dpp, 0);
	regWriteDmu(2, kCmBiasCrR + dpp, 0);
	regWriteDmu(2, kCmBiasYgCbB + dpp, 0);
	regWriteDmu(2, kCmDealpha + dpp, 0);
	regWriteDmu(2, kCmGamcorControl + dpp, 0);
	regWriteDmu(2, kCmHdrMult + dpp, (regReadDmu(2, kCmHdrMult + dpp) & ~0x7ffffu) | kHdrMultOne);
	regWriteDmu(2, kCmControl + dpp, regReadDmu(2, kCmControl + dpp) & ~1u);   // CM_BYPASS = 0, last
	cursorOtgUpdateLock(false);
	uint32_t pend = 0;
	while (pend < 50 && (regReadDmu(2, kCmControl + dpp) & (1u << 8))) {
		IOSleep(1);
		pend++;
	}
	const bool stuck = (regReadDmu(2, kCmControl + dpp) & (1u << 8)) != 0;
	cursorNote("CM written; CM_UPDATE_PENDING %s after %u ms; CM_CONTROL now 0x%08x", stuck ? "STILL SET" : "cleared", pend,
	           regReadDmu(2, kCmControl + dpp));
	cursorCmDump("after the CM write");
	return true;
}

// W45: the normal cursor mode (rdna4-cursor=1, macOS cscSetHardwareCursor/cscDrawHardwareCursor). The real card showed the
// self-test square only after the CM_BYPASS clear of cursorCmApply (boot 4 of round 5: CRC A/B "NO" -> CM write -> "YES"), so the
// same write, with the same OTG-lock bracket and the same identity guard, runs at arming whenever the GOP left the CM
// not amdgpu-like (CM_BYPASS = 1). rdna4-cursorcm=0 is the escape. The self-test (rdna4-cursor=2) does it itself between two CRC
// A/B reads, so it is not repeated there. The dump before and the linuxdiff lines stay in the trail.
void RDNA4Device::cursorCmAuto(const char *why) {
	uint32_t cm = 1;
	PE_parse_boot_argn("rdna4-cursorcm", &cm, sizeof(cm));
	if (cm == 0) {
		cursorNote("%s: rdna4-cursorcm=0: CM_CONTROL.CM_BYPASS left as the GOP left it", why);
		return;
	}
	cursorCmDump(why);
	cursorLinuxDiff(why);
	if (cursorCmApply())
		cursorNote("%s: CM_BYPASS cleared for the macOS cursor path (as amdgpu does when a plane is enabled)", why);
	else
		cursorNote("%s: CM state left unchanged", why);
	cursorLinuxDiff("armed end");
}

// rdna4-cursormpcc=1 (with rdna4-cursor=2): the Linux capture has MPCC0_MPCC_CONTROL = 0xffff0422, the card's GOP leaves 0xffff0461:
// MPCC_MODE 1 (TOP_LAYER_PASSTHROUGH, the state mpc1_remove_mpcc_from_tree leaves, dcn10_mpc.c:309) against 2 (TOP_LAYER_ONLY,
// what mpc1_insert_plane programs for a plane with no bottom layer, dcn10_mpc.c:216), and MPCC_ALPHA_MULTIPLIED_MODE 1 against 0
// (mpc1_update_blending writes it from blnd_cfg->pre_multiplied_alpha, dcn10_mpc.c:84-90). Only those two fields are written
// (RMW), under the OTG update lock, and only if the MPCC has no bottom layer.
bool RDNA4Device::cursorMpccApply() {
	constexpr uint32_t kModeMask = 3u, kAlphaMult = 1u << 6;
	const uint32_t mpcc = pipe.hubp < Pipe::kMaxOtg ? pipe.hubp * kMpccStride : 0;
	const uint32_t ctl = regReadDmu(3, kMpccControl + mpcc), bot = regReadDmu(3, kMpccBotSel + mpcc) & 0xf;
	if (ctl == 0xffffffffu) {
		cursorNote("MPCC write: MPCC_CONTROL reads all ones: nothing written");
		return false;
	}
	if (bot != 0xf) {
		cursorNote("MPCC write: MPCC%u has a bottom layer (BOT_SEL %u): nothing written", pipe.hubp, bot);
		return false;
	}
	if ((ctl & kModeMask) == 2 && !(ctl & kAlphaMult)) {
		cursorNote("MPCC write: MPCC_CONTROL 0x%08x is already TOP_LAYER_ONLY, ALPHA_MULTIPLIED 0: nothing to write", ctl);
		return false;
	}
	cursorNote("MPCC write: MPCC_CONTROL was 0x%08x (MODE %u, ALPHA_MULTIPLIED %u); setting MODE 2 (TOP_LAYER_ONLY), "
	           "ALPHA_MULTIPLIED 0 as amdgpu does (Linux reads 0xffff0422)", ctl, ctl & kModeMask, (ctl >> 6) & 1);
	if (!cursorOtgUpdateLock(true)) {
		cursorNote("MPCC write: the OTG update lock was not held after 10 ms; nothing written");
		return false;
	}
	regWriteDmu(3, kMpccControl + mpcc, (ctl & ~(kModeMask | kAlphaMult)) | 2u);
	cursorOtgUpdateLock(false);
	cursorNote("MPCC written; MPCC_CONTROL now 0x%08x", regReadDmu(3, kMpccControl + mpcc));
	return true;
}

// Frames of the OTG (OTG_STATUS_FRAME_COUNT 0x1b4d [23:0]); bounded, 40 ms per frame.
bool RDNA4Device::cursorWaitFrames(uint32_t n) {
	const uint32_t reg = kOtgFrameCount + otgOff();
	uint32_t last = regReadDmu(2, reg) & 0xffffff;
	for (uint32_t f = 0; f < n; f++) {
		uint32_t waited = 0, now = last;
		while (now == last && waited < 40) {
			IOSleep(1);
			waited++;
			now = regReadDmu(2, reg) & 0xffffff;
		}
		if (now == last)
			return false;
		last = now;
	}
	return true;
}

// OTG CRC A/B (rootcause-cursor.md section 3). dcn401 uses optc1_configure_crc / optc1_get_crc
// (dcn401_optc.c:507-508, dcn10_optc.c:1465-1576): the windows, then OTG_CRC_CNTL CONT_EN, CRC0_SELECT and EN.
// Registers (dcn_4_1_0_offset.h base idx 2, + otgOff()): OTG_CRC_CNTL 0x1b65 (EN 0, CONT_EN 4, CRC0_SELECT [22:20]),
// OTG_CRC0_WINDOWA_X/Y_CONTROL 0x1b66/0x1b67 (START [14:0], END [30:16]), WINDOWB_X/Y 0x1b68/0x1b69,
// OTG_CRC0_DATA_RG 0x1b6a (R [15:0], G [31:16]), OTG_CRC0_DATA_B 0x1b6b (B [15:0]; [31:16] is CRC0_C, never read by DC).
// The window is the 64x64 square at (100,100) shrunk by kCrcMargin on each side: whether END is inclusive is not
// stated in the tree, and an edge pixel of the scrolling verbose console must not enter an "on" read (W38 review S1).
// The CRC is read with the cursor on, off, on, off. The sprite is opaque and covers the window, so an "on" CRC
// is a constant that does not depend on the desktop; only the "off" reads vary while the console scrolls:
//   YES  = on0 == on2, and on0 differs from BOTH off reads (the off reads may differ from each other)
//   NO   = on0 equals both off reads (the sprite adds nothing at the OTG)
//   else INCONCLUSIVE. With a static desktop this is the plain A/B rule.
// Each DATA_RG/DATA_B sample is read twice and repeated (<= 3 tries) until both agree, against a pair torn across a frame.
const char *RDNA4Device::cursorCrcCheck(const char *label) {
	const uint32_t otg = otgOff();
	constexpr uint32_t kCrcMargin = 4;
	const uint32_t x0 = 100 + kCrcMargin, x1 = 100 + kCursorWidth - kCrcMargin;
	const uint32_t y0 = 100 + kCrcMargin, y1 = 100 + kCursorHeight - kCrcMargin;
	const uint32_t x = x0 | (x1 << 16), y = y0 | (y1 << 16);
	regWriteDmu(2, kOtgCrcWindowAX + otg, x);
	regWriteDmu(2, kOtgCrcWindowAY + otg, y);
	regWriteDmu(2, kOtgCrcWindowBX + otg, x);
	regWriteDmu(2, kOtgCrcWindowBY + otg, y);
	const uint32_t cntl = regReadDmu(2, kOtgCrcCntl + otg);
	regWriteDmu(2, kOtgCrcCntl + otg, (cntl & ~(0x7u << 20)) | (1u << 4) | 1u);   // SELECT 0, CONT_EN, EN
	// what latched: a GOP that left WINDOW_DB_EN set would show here as windows that are not the ones written
	cursorNote("CRC programmed: OTG_CRC_CNTL 0x%08x (was 0x%08x) winA x 0x%08x y 0x%08x winB x 0x%08x y 0x%08x "
	           "(wrote x 0x%08x y 0x%08x)", regReadDmu(2, kOtgCrcCntl + otg), cntl,
	           regReadDmu(2, kOtgCrcWindowAX + otg), regReadDmu(2, kOtgCrcWindowAY + otg),
	           regReadDmu(2, kOtgCrcWindowBX + otg), regReadDmu(2, kOtgCrcWindowBY + otg), x, y);
	if (!(regReadDmu(2, kOtgCrcCntl + otg) & 1)) {
		cursorNote("CRC A/B [%s]: OTG_CRC_CNTL.EN did not stick: no verdict", label);
		return "INCONCLUSIVE";
	}
	struct Crc { uint32_t rg, b; } c[4];
	bool frames = true;
	uint32_t torn = 0;
	for (int i = 0; i < 4; i++) {
		const bool on = (i & 1) == 0;
		if (i > 0)
			cursorProgramPlane(on);   // attributes with CURSOR_ENABLE / CUR0_ENABLE set as asked, waits for the latch
		frames = cursorWaitFrames(3) && frames;
		Crc a { 0, 0 }, b { 0, 0 };
		bool agreed = false;
		for (int t = 0; t < 3 && !agreed; t++) {
			a.rg = regReadDmu(2, kOtgCrcDataRg + otg);
			a.b = regReadDmu(2, kOtgCrcDataB + otg) & 0xffff;
			b.rg = regReadDmu(2, kOtgCrcDataRg + otg);
			b.b = regReadDmu(2, kOtgCrcDataB + otg) & 0xffff;
			agreed = a.rg == b.rg && a.b == b.b;
		}
		if (!agreed)
			torn++;
		c[i] = b;
	}
	// the plane is back on (i = 3 was "off": switch it on again) and the CRC engine is released
	cursorProgramPlane(true);
	regWriteDmu(2, kOtgCrcCntl + otg, regReadDmu(2, kOtgCrcCntl + otg) & ~1u);
	auto same = [](const Crc &a, const Crc &b) { return a.rg == b.rg && a.b == b.b; };
	const char *verdict;
	if (!frames)
		verdict = "INCONCLUSIVE (the OTG frame counter did not advance)";
	else if (!c[0].rg && !c[0].b && !c[1].rg && !c[1].b && !c[2].rg && !c[2].b && !c[3].rg && !c[3].b)
		verdict = "INCONCLUSIVE (every CRC reads 0: the engine is not counting)";
	else if (same(c[0], c[2]) && !same(c[0], c[1]) && !same(c[0], c[3]))
		verdict = "YES";
	else if (same(c[0], c[1]) && same(c[0], c[3]))
		verdict = "NO";
	else if (!same(c[0], c[2]))
		verdict = "INCONCLUSIVE (the two 'on' reads differ: the window is not covered by the square)";
	else
		verdict = "INCONCLUSIVE (one 'off' read equals 'on', the other does not)";
	cursorNote("CRC A/B [%s] (%u,%u)..(%u,%u) R.G/B: on %04x.%04x/%04x off %04x.%04x/%04x on %04x.%04x/%04x "
	           "off %04x.%04x/%04x torn %u: cursor pixels reach the output: %s",
	           label, x0, y0, x1, y1,
	           c[0].rg & 0xffff, c[0].rg >> 16, c[0].b, c[1].rg & 0xffff, c[1].rg >> 16, c[1].b,
	           c[2].rg & 0xffff, c[2].rg >> 16, c[2].b, c[3].rg & 0xffff, c[3].rg >> 16, c[3].b, torn, verdict);
	return verdict[0] == 'Y' ? "YES" : verdict[0] == 'N' ? "NO" : "INCONCLUSIVE";
}

// rdna4-cursor=2: at arming, show an opaque magenta 64x64 square at (100,100)
// through the same programming path macOS's cursor uses. Whether it appears on the
// card is a yes/no answer that does not depend on macOS's cursor image (which can
// be blank early on) or its calls. macOS's first cscSetHardwareCursor replaces it.
void RDNA4Device::cursorSelfTest() {
	for (uint32_t i = 0; i < kCursorWidth * kCursorHeight; i++)
		cursorVram[i] = 0xffff00ffu;   // premultiplied ARGB: opaque magenta
	const uint32_t hubp = hubpOff();
	// One lock bracket around position and attributes: they latch together at the unlock, so
	// the square is never armed at (0,0) (the order inside a bracket does not matter).
	cursorDsclDump("selftest pre");   // W38: what the GOP left in the DSCL / HUBP request state, before any write
	cursorCmDump("selftest pre");     // W40: CM0_CM_CONTROL.CM_BYPASS and the CM sub-blocks the GOP left
	cursorDlgDump("selftest pre");    // W40: the DLG/TTU registers 0x063b-0x0655
	cursorLinuxDiff("pre");           // W40: everything static in the pipe that differs from the Linux capture, before any write
	cursorPipeFixes();                // W38: CRQ_EXPANSION_MODE, cursor memory power, USE_MALL_FOR_CURSOR per update
	cursorDsclDecide();               // W38: DSCL_MODE / RECOUT / MPC_SIZE verdict (+ amdgpu's mode-0 set with rdna4-cursordscl=1)
	cursorProgramMissionMode();   // W34: amdgpu's HUBPREQ_DEBUG_DB = 1 << 8, before the cursor is enabled
	cursorMpcLock(true);
	cursorProgramTtu();   // W32, opt-in (rdna4-cursorttu=1): legacy-DML request scheduling, latched with the rest
	regWriteDmu(2, kCursorPosition + hubp, 100u | (100u << 15));
	regWriteDmu(2, kCursorDstOffset + hubp, cursorDstXOffset(100));
	cursorProgramPlane(true);   // nested: does not touch the lock register
	cursorMpcLock(false);
	cursorWaitLatched("selftest", 40);
	cursorNote("SELF-TEST (rdna4-cursor=2): an opaque magenta 64x64 square is programmed at (100,100)");
	cursorNote("self-test state: hwCursorVisible %d hwCursorEnabledHw %d (macOS calls are ignored)", hwCursorVisible, hwCursorEnabledHw);
	cursorDumpState("selftest");
	cursorHold = true;
	hwCursorSet = true;
	cursorRegProbe("selftest");
	cursorDsclDump("selftest post");
	// W38: does the square reach the OTG? decided by the OTG CRC with the cursor on/off/on/off
	const char *before = cursorCrcCheck("GOP CM state");
	const char *last = before;
	uint32_t cm = 1;   // W45: on by default, rdna4-cursorcm=0 leaves the GOP's CM_BYPASS alone
	PE_parse_boot_argn("rdna4-cursorcm", &cm, sizeof(cm));
	if (cm != 0) {
		// W40: the same boot attributes the CM change: CRC before (above) and after
		if (cursorCmApply()) {
			const char *after = cursorCrcCheck("after CM enable");
			cursorNote("CM bypass: before %s, after %s", before, after);
			last = after;
		} else {
			cursorNote("CM bypass: before %s, after not run (CM state unchanged)", before);
		}
	} else {
		cursorNote("CM bypass: not changed (rdna4-cursorcm=0 leaves the GOP's CM state); CRC %s", before);
	}
	uint32_t mp = 0;
	if (PE_parse_boot_argn("rdna4-cursormpcc", &mp, sizeof(mp)) && mp == 1) {
		if (cursorMpccApply()) {
			const char *after = cursorCrcCheck("after MPCC mode");
			cursorNote("MPCC mode: before %s, after %s", last, after);
		} else {
			cursorNote("MPCC mode: before %s, after not run (MPCC state unchanged)", last);
		}
	}
	cursorLinuxDiff("end");   // W40: what still differs from the Linux capture after the writes above
}

// One line of everything that decides whether the plane shows: the HUBP
// cursor registers, the DPP/CM side (CUR0_UPDATE_PENDING is bit 16), and the
// OTG's double-buffer lock and global sync state (why a write would not latch).
void RDNA4Device::cursorDumpState(const char *why) {
	const uint32_t hubp = hubpOff(), dpp = dppOff(), otg = otgOff();
	const uint32_t cm = regReadDmu(2, kCursorCmControl + dpp);
	cursorNote("%s: hubp ctl=0x%08x addr=0x%08x/%04x size=0x%08x pos=0x%08x hot=0x%08x "
	           "dst=0x%08x set=0x%08x", why,
	           regReadDmu(2, kCursorControl + hubp), regReadDmu(2, kCursorAddress + hubp),
	           regReadDmu(2, kCursorAddressHigh + hubp) & 0xffff,
	           regReadDmu(2, kCursorSize + hubp), regReadDmu(2, kCursorPosition + hubp),
	           regReadDmu(2, kCursorHotSpot + hubp), regReadDmu(2, kCursorDstOffset + hubp),
	           regReadDmu(2, kCursorSettings + hubp));
	cursorNote("%s: cm ctl=0x%08x (update pending %u) fp=0x%08x/0x%08x mtx=0x%08x | "
	           "otg lock=0x%08x dbuf=0x%08x sync=0x%08x", why, cm, (cm >> 16) & 1,
	           regReadDmu(2, kCursorCmScaleGY + dpp), regReadDmu(2, kCursorCmScaleRB + dpp),
	           regReadDmu(2, kCursorCmMatrix + dpp),
	           regReadDmu(2, kOtgMasterUpdateLock + otg), regReadDmu(2, kOtgDoubleBufferCtl + otg),
	           regReadDmu(2, kOtgGlobalSyncStatus + otg));
	const uint32_t cnvc = regReadDmu(2, kCnvcFormatControl + dpp);
	const uint32_t cmc = regReadDmu(2, kCmControl + dpp);
	const uint32_t mpcc = pipe.hubp < Pipe::kMaxOtg ? pipe.hubp * kMpccStride : 0;
	cursorNote("%s: latch domains: cnvc fmt=0x%08x (pending %u) cm ctl=0x%08x (pending %u) dpp top=0x%08x | "
	           "mpcc ctl=0x%08x lock_sel=0x%08x status=0x%08x out mux=0x%08x", why, cnvc, (cnvc >> 20) & 1,
	           cmc, (cmc >> 8) & 1, regReadDmu(2, kDppTopControl + dpp),
	           regReadDmu(3, kMpccControl + mpcc), regReadDmu(3, kMpccUpdateLockSel + mpcc),
	           regReadDmu(3, kMpccStatus + mpcc),
	           regReadDmu(3, kMpcOutMux + (pipe.otg < Pipe::kMaxOtg ? pipe.otg * kMpcOutStride : 0)));
	cursorLockNote(why);
}

// The cursor lock of this pipe's OPP: MPC seg 3, kMpcLockBase + 5 * opp + 4. amdgpu writes
// 1 before a cursor update and 0 after (dc_stream.c:313,332 / 476,491 -> dcn10_cursor_lock ->
// mpc1_cursor_lock); while it is 1 the HUBP/DPP cursor registers stay pending. Nested calls
// (the self-test wraps position + attributes in one bracket) count, and only the outer pair
// touches the register.
void RDNA4Device::cursorMpcLock(bool lock) {
	if (!cursorUseLock)
		return;
	const uint32_t opp = pipe.opp < Pipe::kMaxOtg ? pipe.opp : 0;
	const uint32_t reg = kMpcLockBase + kMpcLockStride * opp + kMpcCurLock;
	if (lock) {
		if (cursorLockDepth++ == 0)
			regWriteDmu(3, reg, 1);
	} else if (cursorLockDepth > 0 && --cursorLockDepth == 0) {
		regWriteDmu(3, reg, 0);
		if (cursorGopHeld) {   // the lock the GOP left held ends here, after the first programming
			cursorGopHeld = false;
			cursorNote("released the GOP-held CUR_VUPDATE_LOCK_SET at the end of the first bracket "
			           "(reads 0x%08x now)", regReadDmu(3, reg));
		}
	}
}

// Every lock-set register of this OPP and the MPC's pending status: the picture that says
// which lock, if any, is holding the cursor update back.
void RDNA4Device::cursorLockNote(const char *why) {
	const uint32_t opp = pipe.opp < Pipe::kMaxOtg ? pipe.opp : 0;
	const uint32_t b = kMpcLockBase + kMpcLockStride * opp;
	cursorNote("%s: mpc locks opp%u: adr_cfg_cur=0x%08x adr_cfg=0x%08x adr=0x%08x cfg=0x%08x CUR=0x%08x | "
	           "dpp pending=0x%08x misc pending=0x%08x", why, opp, regReadDmu(3, b), regReadDmu(3, b + 1),
	           regReadDmu(3, b + 2), regReadDmu(3, b + 3), regReadDmu(3, b + kMpcCurLock),
	           regReadDmu(3, kMpcDppPending), regReadDmu(3, kMpcPendingMisc));
}

// After the unlock the cursor registers latch at the next VUPDATE (at most one frame, ~17 ms):
// poll CM_CUR0's CUR0_UPDATE_PENDING (bit 16) for up to maxMs and say how it went, for the first
// few calls (evidence for the card; the wait is bounded and only used off the per-move path).
bool RDNA4Device::cursorWaitLatched(const char *why, uint32_t maxMs) {
	const uint32_t reg = kCursorCmControl + dppOff();
	uint32_t waited = 0, cm = regReadDmu(2, reg);
	while ((cm >> 16) & 1 && waited < maxMs && cm != 0xffffffffu) {
		IOSleep(1);
		waited++;
		cm = regReadDmu(2, reg);
	}
	const bool latched = !((cm >> 16) & 1);
	if (cursorLatchLogs < 10) {
		cursorLatchLogs++;
		cursorNote("%s: cursor update %s after %u ms (cm ctl 0x%08x)", why,
		           latched ? "LATCHED" : "still PENDING", waited, cm);
	}
	return latched;
}

bool RDNA4Device::initHardwareCursor() {
	if (!cursorTrailLock)
		cursorTrailLock = IOLockAlloc();
	if (cursorTrailLock)   // the compute thread's flip hook may append at the same time (W32 review NIT)
		IOLockLock(cursorTrailLock);
	cursorTrail[0] = '\0';
	cursorTrailLen = 0;
	cursorTrailFull = false;
	if (cursorTrailLock)
		IOLockUnlock(cursorTrailLock);
	if (!hwCursorRequested || !isAmd || !ipDiscovery.isValid() || !rmmio ||
	    !fbPhysBase || !fbLength || pipe.hubp >= Pipe::kMaxOtg) {
		cursorNote("unavailable (requested %d amd %d discovery %d mmio %d fb 0x%llx+0x%llx hubp %u); "
		           "staying with software cursor", hwCursorRequested, isAmd,
		           ipDiscovery.isValid(), rmmio != nullptr,
		           static_cast<unsigned long long>(fbPhysBase),
		           static_cast<unsigned long long>(fbLength), pipe.hubp);
		return false;
	}

	const uint32_t hubp = hubpOff();
	const uint64_t scanout = static_cast<uint64_t>(regReadDmu(2, 0x060a + hubp)) |
	                         (static_cast<uint64_t>(regReadDmu(2, 0x060b + hubp) & 0xffff) << 32);
	if (!scanout || scanout == 0xffffffffffffffffull) {
		cursorNote("scanout address unavailable; staying with software cursor");
		return false;
	}

	// Keep the sprite in the same CPU-visible VRAM aperture as the scanout,
	// after the console allocation and on a page boundary.
	cursorMcAddr = (scanout + fbLength + 0xfff) & ~0xfffull;
	const uint64_t delta = cursorMcAddr - scanout;
	if (delta + kCursorBytes > 192ull * 1024 * 1024) {
		cursorNote("sprite offset 0x%llx outside the safe aperture", delta);
		return false;
	}

	IODeviceMemory *memory = IODeviceMemory::withRange(fbPhysBase + delta, kCursorBytes);
	if (!memory)
		return false;
	cursorMap = memory->map();
	memory->release();
	if (!cursorMap) {
		cursorNote("VRAM sprite mapping failed; staying with software cursor");
		return false;
	}
	cursorVram = reinterpret_cast<volatile uint32_t *>(cursorMap->getVirtualAddress());
	cursorStage = static_cast<uint32_t *>(IOMalloc(kCursorBytes));
	if (!cursorVram || !cursorStage) {
		freeHardwareCursor();
		return false;
	}
	memset(cursorStage, 0, kCursorBytes);
	for (uint32_t i = 0; i < kCursorWidth * kCursorHeight; i++)
		cursorVram[i] = 0;
	ensureUpdateLatch();
	// The GOP never used a cursor and has no reason to leave the MPC cursor lock in a defined
	// state. amdgpu only ever writes it around an update (mpc1_cursor_lock); a lock left at 1
	// holds every cursor write pending forever (the symptom the card showed:
	// CUR0_UPDATE_PENDING stuck at 1; the value the GOP left is what this reads). Release a held lock.
	{
		const uint32_t opp = pipe.opp < Pipe::kMaxOtg ? pipe.opp : 0;
		const uint32_t lockReg = kMpcLockBase + kMpcLockStride * opp + kMpcCurLock;
		const uint32_t held = regReadDmu(3, lockReg);
		cursorLockNote("armed, as the GOP left it");
		uint32_t useLock = 1;
		if (PE_parse_boot_argn("rdna4-cursorlock", &useLock, sizeof(useLock)) && useLock == 0) {
			cursorUseLock = false;
			cursorNote("rdna4-cursorlock=0: the MPC cursor lock is left alone (the pre-W25 behaviour)");
		}
		// Not released here (W25 review S1): between a release and the first programming the
		// next VUPDATE would latch whatever the cursor registers hold (GOP or reset state). The
		// first bracket of cursorMpcLock re-asserts the lock, programs, and its unlock is what
		// releases a held one, so only programmed state ever latches.
		if (cursorUseLock && held != 0xffffffffu && (held & 1)) {
			cursorGopHeld = true;
			cursorNote("CUR_VUPDATE_LOCK_SET is held (0x%08x) as the GOP left it; the first cursor bracket releases it", held);
		}
	}
	hwCursorReady = true;
	{
		const uint32_t timer = regReadDmu(2, kDchubGlobalTimerCntl);
		const char *src = "DCHUBBUB_GLOBAL_TIMER_CNTL, DCCG ref assumed 100 MHz";
		uint32_t khz = 0;
		if (timer != 0xffffffffu && (timer & (1u << 12)))
			khz = (timer & 0xf) == 2 ? 50000 : 100000;
		if (khz < 40000 || khz > 60000) {
			khz = 50000;
			src = "timer disabled or out of DC's 40-60 MHz range: DC's ~50 MHz";
		}
		cursorRefClkKHz = khz;
		cursorNote("DCHUB ref clock %u kHz for CURSOR_DST_X_OFFSET (%s; timer cntl 0x%08x); pixel clock %u kHz%s",
		           khz, src, timer, bootTimingValid ? bootTiming.pixelClockKHz : 0,
		           bootTimingValid && bootTiming.pixelClockKHz ? "" : " (unknown: DST_X_OFFSET written as 0)");
	}
	cursorNote("NDRV hardware cursor ready: HUBP%u DPP%u OTG%u, scanout MC 0x%llx (fb len 0x%llx), "
	           "sprite MC 0x%llx = cpu 0x%llx (%ux%u slot)", pipe.hubp, pipe.hubp, pipe.otg,
	           scanout, static_cast<unsigned long long>(fbLength), cursorMcAddr,
	           static_cast<unsigned long long>(fbPhysBase + delta), kCursorWidth, kCursorHeight);
	// Where cursor fetches go: an address outside these windows returns zeros
	// without latching any error (the reason a sprite can be armed and invisible).
	cursorNote("vm: fb_loc base=0x%08x top=0x%08x agp bot=0x%08x top=0x%08x base=0x%08x "
	           "sys_ap low=0x%08x high=0x%08x l1_tlb=0x%08x",
	           regReadDmu(2, kVmFbBase), regReadDmu(2, kVmFbTop), regReadDmu(2, kVmAgpBot),
	           regReadDmu(2, kVmAgpTop), regReadDmu(2, kVmAgpBase),
	           regReadDmu(2, kSysApLow + hubp), regReadDmu(2, kSysApHigh + hubp),
	           regReadDmu(2, kL1TlbCntl + hubp));
	cursorDumpState("armed");
	cursorRegProbe("armed");
	uint32_t mode = 0;
	if (PE_parse_boot_argn("rdna4-cursor", &mode, sizeof(mode)) && mode == 2)
		cursorSelfTest();
	else
		cursorCmAuto("armed pre");   // W45: the real macOS cursor path needs the CM out of bypass as well
	return true;
}

void RDNA4Device::freeHardwareCursor() {
	hwCursorReady = false;
	hwCursorSet = false;
	hwCursorVisible = false;
	cursorVram = nullptr;
	if (cursorMap) {
		cursorMap->release();
		cursorMap = nullptr;
	}
	if (cursorStage) {
		IOFree(cursorStage, kCursorBytes);
		cursorStage = nullptr;
	}
}

IOReturn RDNA4Device::setHardwareCursor(void *cursorRef) {
	if (!hwCursorReady || !cursorRef || !cursorStage || !cursorVram)
		return kIOReturnUnsupported;
	if (!cursorProbedSet) {   // W32: the plane's state when macOS first talks to it
		cursorProbedSet = true;
		cursorRegProbe("first cscSetHardwareCursor");
	}
	if (cursorHold) {   // rdna4-cursor=2: keep the test square
		hwCursorSet = true;
		if (++cursorHeldCalls <= 3)
			cursorNote("self-test: cscSetHardwareCursor ignored, macOS calls the hardware cursor path");
		return kIOReturnSuccess;
	}

	IOHardwareCursorDescriptor descriptor {};
	descriptor.majorVersion = kHardwareCursorDescriptorMajorVersion;
	descriptor.minorVersion = kHardwareCursorDescriptorMinorVersion;
	descriptor.width = kCursorWidth;
	descriptor.height = kCursorHeight;
	descriptor.bitDepth = 32;
	IOHardwareCursorInfo info {};
	info.majorVersion = kHardwareCursorInfoMajorVersion;
	info.minorVersion = kHardwareCursorInfoMinorVersion;
	info.hardwareCursorData = reinterpret_cast<UInt8 *>(cursorStage);
	if (!Ndrv::prepareCursor(cursorRef, &descriptor, &info) ||
	    !info.cursorWidth || !info.cursorHeight || info.cursorWidth > kCursorWidth ||
	    info.cursorHeight > kCursorHeight) {
		cursorNote("VSLPrepareCursorForHardwareCursor refused the image (%ux%u)",
		           info.cursorWidth, info.cursorHeight);
		return kIOReturnUnsupported;
	}

	cursorWidth = info.cursorWidth;
	cursorHeight = info.cursorHeight;
	hwCursorHotX = info.cursorHotSpotX;
	hwCursorHotY = info.cursorHotSpotY;
	// The image is tightly packed (cursorWidth per row) in the staging buffer;
	// the VRAM slot has a fixed 64-pixel pitch and everything outside the image
	// stays transparent.
	uint32_t maxAlpha = 0, opaqueIdx = 0;
	for (uint32_t i = 0; i < cursorWidth * cursorHeight; i++) {
		cursorStage[i] = premultiplyArgb(cursorStage[i]);
		if ((cursorStage[i] >> 24) > maxAlpha) {
			maxAlpha = cursorStage[i] >> 24;
			opaqueIdx = i;
		}
	}
	for (uint32_t i = 0; i < kCursorWidth * kCursorHeight; i++)
		cursorVram[i] = 0;
	for (uint32_t y = 0; y < cursorHeight; y++)
		for (uint32_t x = 0; x < cursorWidth; x++)
			cursorVram[y * kCursorPitch + x] = cursorStage[y * cursorWidth + x];
	// Data path check: the most opaque pixel must read back from VRAM. A
	// fully transparent conversion (max alpha 0) or a mismatch explains an
	// invisible sprite without any register being wrong.
	const uint32_t rbIdx = (opaqueIdx / cursorWidth) * kCursorPitch + (opaqueIdx % cursorWidth);
	cursorNote("image %ux%u hot %u,%u: max alpha 0x%02x stage[%u]=0x%08x vram=0x%08x %s",
	           cursorWidth, cursorHeight, hwCursorHotX, hwCursorHotY, maxAlpha, opaqueIdx,
	           cursorStage[opaqueIdx], cursorVram[rbIdx],
	           cursorStage[opaqueIdx] == cursorVram[rbIdx] ? "(match)" : "(MISMATCH)");

	cursorProgramPlane(hwCursorVisible);
	hwCursorSet = true;
	cursorDumpState("set");
	return kIOReturnSuccess;
}

IOReturn RDNA4Device::drawHardwareCursor(int32_t x, int32_t y, uint32_t visible) {
	if (!hwCursorReady || !hwCursorSet)
		return kIOReturnUnsupported;
	if (!cursorProbedDraw) {
		cursorProbedDraw = true;
		cursorRegProbe("first cscDrawHardwareCursor");
	}
	if (cursorHold) {   // rdna4-cursor=2: the test square stays, macOS's pointer is not programmed
		hwCursorX = x;
		hwCursorY = y;
		hwCursorVisible = visible != 0;
		if (++cursorHeldCalls <= 3)
			cursorNote("self-test: cscDrawHardwareCursor #%u (%d,%d visible %u) ignored", cursorHeldCalls, x, y, visible);
		return kIOReturnSuccess;
	}
	hwCursorX = x;
	hwCursorY = y;
	hwCursorVisible = visible != 0;
	cursorMpcLock(true);   // dcn10_cursor_lock before the position/enable writes (dc_stream.c:476)
	const uint32_t hubp = hubpOff();
	const uint32_t dpp = dppOff();

	// x,y is the image's top-left corner. Where it lies above or left of the
	// screen, position pins at 0 and the hot spot register carries the excess
	// (hubp401_cursor_set_position: the plane starts at position - hot spot).
	const uint32_t px = x > 0 ? static_cast<uint32_t>(x) & 0x7fff : 0;
	const uint32_t py = y > 0 ? static_cast<uint32_t>(y) & 0x7fff : 0;
	const uint32_t hx = x < 0 ? static_cast<uint32_t>(-x < 255 ? -x : 255) : 0;
	const uint32_t hy = y < 0 ? static_cast<uint32_t>(-y < 255 ? -y : 255) : 0;
	regWriteDmu(2, kCursorPosition + hubp, py | (px << 15));
	regWriteDmu(2, kCursorHotSpot + hubp, hy | (hx << 16));
	// CURSOR_DST_X_OFFSET is the fetch deadline in refclk time: x scaled from
	// pixel clock to refclk (hubp401_cursor_set_position).
	regWriteDmu(2, kCursorDstOffset + hubp, cursorDstXOffset(px));

	// Like amdgpu, touch the enables only when the visibility changes:
	// rewriting the double-buffered control registers on every move re-arms
	// CUR0_UPDATE_PENDING each time (hubp401: if cur_enable != cur_en).
	bool changed = false;
	if (hwCursorVisible != hwCursorEnabledHw) {
		regWriteDmu(2, kCursorControl + hubp, cursorCtlBase | (hwCursorVisible ? 1u : 0u));
		regWriteDmu(2, kCursorCmControl + dpp,
		            kCursorCmWorkingBits | (kCursorModePremultipliedArgb << kCursorCmModeShift) |
		            (hwCursorVisible ? 1u : 0u));
		hwCursorEnabledHw = hwCursorVisible;
		changed = true;
	}
	cursorMpcLock(false);   // program_cursor_position: unlock; the update latches at the next VUPDATE
	cursorDrawCalls++;
	if (changed && cursorVisChanges < 6) {
		cursorVisChanges++;
		cursorWaitLatched(hwCursorVisible ? "shown" : "hidden", 40);
		cursorDumpState(hwCursorVisible ? "shown" : "hidden");
	} else if (cursorDrawCalls <= 3) {
		cursorWaitLatched("move", 40);
	}
	// The first moves, then a slow sample: it shows whether CUR0_UPDATE_PENDING
	// ever clears and whether the position registers hold what was written.
	if (cursorDrawLogs < 5 || (cursorDrawLogs < 12 && (cursorDrawCalls & 0x1ff) == 0)) {
		cursorDrawLogs++;
		cursorDumpState("move");
		cursorNote("move #%u x=%d y=%d visible=%u", cursorDrawCalls, x, y, visible);
	}
	return kIOReturnSuccess;
}

IOReturn RDNA4Device::getHardwareCursorDrawState(
	Ndrv::VDHardwareCursorDrawStateRec &state) const {
	state.csCursorX = hwCursorX;
	state.csCursorY = hwCursorY;
	state.csCursorVisible = hwCursorVisible ? 1u : 0u;
	state.csCursorSet = hwCursorSet ? 1u : 0u;
	state.csReserved1 = 0;
	state.csReserved2 = 0;
	return kIOReturnSuccess;
}
