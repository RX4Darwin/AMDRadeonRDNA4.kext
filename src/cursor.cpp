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
constexpr uint32_t kTtuQosFixedCur0 = 8;   // dml_display_rq_dlg_calc.c:497 qos_level_fixed_cur0 = 8, ramp not disabled (:500)
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

// W32, rdna4-cursor=2 only (call inside the lock bracket): program the cursor's request scheduling the way DC
// does from DML, DCN_CUR0_TTU_CNTL0/1 (hubp401_program_deadline dcn401_hubp.c:406-409, _setup_interdependent
// :473-474). DML (display_mode_core.c:3441-3448, VRatio <= 1, HRatio 1) delivers one cursor request every
//   CursorRequestDeliveryTime [us] = width / pixel clock [MHz] / cursor_req_per_width,
//   cursor_req_per_width = ceil(width * 32 bpp / 256 / 8) = 1 for the 64 px slot;
// dml_display_rq_dlg_calc.c:403-404,486-487 turns it into DCHUB ref cycles times 2^10, with QoS_LEVEL_FIXED 8 and
// the ramp enabled (:497,:500); the prefetch pair (_PRE) is the same value while VRatioPrefetchY <= 1. The
// pixel clock is the boot timing's, the ref clock the DCHUB one read at arming. Without a pixel clock the value
// falls back to the surface's own SURF0 delivery value (the analysis' first cheap test). rdna4-cursorttu=0
// skips it (the A/B control: same dump, no write).
bool RDNA4Device::cursorProgramTtu() {
	uint32_t enable = 1;
	if (PE_parse_boot_argn("rdna4-cursorttu", &enable, sizeof(enable)) && enable == 0) {
		cursorNote("rdna4-cursorttu=0: DCN_CUR0_TTU_CNTL0/1 are left alone (control)");
		return false;
	}
	const uint32_t hubp = hubpOff();
	const uint32_t pixelKHz = bootTimingValid ? bootTiming.pixelClockKHz : 0;
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
	cursorNote("CUR0 TTU programmed: CNTL0 0x%08x CNTL1 0x%08x (delivery %u = 0x%x ref cycles * 2^10; pixel clock %u kHz, "
	           "ref clock %u kHz; %s); reads back 0x%08x/0x%08x", cntl0, delivery, delivery, delivery, pixelKHz,
	           cursorRefClkKHz, source, regReadDmu(2, kTtuCur0Cntl0 + hubp), regReadDmu(2, kTtuCur0Cntl1 + hubp));
	return true;
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
	cursorMpcLock(true);
	cursorProgramTtu();   // W32: DML-style request scheduling for the plane, latched with the rest
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
	cursorTrail[0] = '\0';
	cursorTrailLen = 0;
	cursorTrailFull = false;
	if (!cursorTrailLock)
		cursorTrailLock = IOLockAlloc();
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
