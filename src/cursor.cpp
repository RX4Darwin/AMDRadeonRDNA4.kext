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
// NDRV hardware cursor (W14)
// ---------------------------------------------------------------------------

#include "device.hpp"
#include "ndrv.hpp"

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

constexpr uint32_t kCursorWidth = 64;
constexpr uint32_t kCursorHeight = 64;
constexpr uint32_t kCursorPitch = 64;
constexpr uint32_t kCursorBytes = kCursorWidth * kCursorHeight * sizeof(uint32_t);
constexpr uint32_t kCursorModePremultipliedArgb = 2; // dc_cursor_color_format
constexpr uint32_t kCursorFp16One = 0x3c00;
constexpr uint32_t kCursorReqModePrefetch = 1u << 2;
constexpr uint32_t kCursorModeShift = 8;
constexpr uint32_t kCursorPitchShift = 16;
constexpr uint32_t kCursorLinesPerChunkShift = 24;
constexpr uint32_t kCursorCmModeShift = 4;
constexpr uint32_t kCursorCmWorkingBits = (1u << 7) | (1u << 2);

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

bool RDNA4Device::initHardwareCursor() {
	if (!hwCursorRequested || !isAmd || !ipDiscovery.isValid() || !rmmio ||
	    !fbPhysBase || !fbLength || pipe.hubp >= Pipe::kMaxOtg) {
		FBLOG("cursor: unavailable; staying with software cursor");
		return false;
	}

	const uint32_t hubp = hubpOff();
	const uint64_t scanout = static_cast<uint64_t>(regReadDmu(2, 0x060a + hubp)) |
	                         (static_cast<uint64_t>(regReadDmu(2, 0x060b + hubp) & 0xffff) << 32);
	if (!scanout || scanout == 0xffffffffffffffffull) {
		FBLOG("cursor: scanout address unavailable; staying with software cursor");
		return false;
	}

	// Keep the sprite in the same CPU-visible VRAM aperture as the scanout,
	// after the console allocation and on a page boundary.
	cursorMcAddr = (scanout + fbLength + 0xfff) & ~0xfffull;
	const uint64_t delta = cursorMcAddr - scanout;
	if (delta + kCursorBytes > 192ull * 1024 * 1024) {
		FBLOG("cursor: sprite offset 0x%llx outside the safe aperture", delta);
		return false;
	}

	IODeviceMemory *memory = IODeviceMemory::withRange(fbPhysBase + delta, kCursorBytes);
	if (!memory)
		return false;
	cursorMap = memory->map();
	memory->release();
	if (!cursorMap) {
		FBLOG("cursor: VRAM sprite mapping failed; staying with software cursor");
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
	hwCursorReady = true;
	FBLOG("cursor: NDRV hardware cursor ready, HUBP%u sprite MC 0x%llx (%ux%u max)",
	      pipe.hubp, cursorMcAddr, kCursorWidth, kCursorHeight);
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
		FBLOG("cursor: VSLPrepareCursorForHardwareCursor refused image");
		return kIOReturnUnsupported;
	}

	cursorWidth = info.cursorWidth;
	cursorHeight = info.cursorHeight;
	hwCursorHotX = info.cursorHotSpotX;
	hwCursorHotY = info.cursorHotSpotY;
	for (uint32_t i = 0; i < kCursorWidth * kCursorHeight; i++)
		cursorStage[i] = premultiplyArgb(cursorStage[i]);
	for (uint32_t i = 0; i < kCursorWidth * kCursorHeight; i++)
		cursorVram[i] = 0;
	for (uint32_t y = 0; y < cursorHeight; y++)
		for (uint32_t x = 0; x < cursorWidth; x++)
			cursorVram[y * kCursorPitch + x] = cursorStage[y * cursorWidth + x];

	ensureUpdateLatch();
	const uint32_t hubp = hubpOff();
	const uint32_t dpp = dppOff();
	regWriteDmu(2, kCursorAddressHigh + hubp, static_cast<uint32_t>(cursorMcAddr >> 32) & 0xffff);
	regWriteDmu(2, kCursorAddress + hubp, static_cast<uint32_t>(cursorMcAddr));
	regWriteDmu(2, kCursorSize + hubp, cursorHeight | (cursorWidth << 16));
	regWriteDmu(2, kCursorHotSpot + hubp, hwCursorHotY | (static_cast<uint32_t>(hwCursorHotX) << 16));
	regWriteDmu(2, kCursorSettings + hubp, 3u << 8); // CHUNK_HDL_ADJUST=3
	const uint32_t hubpControl = kCursorReqModePrefetch |
		(kCursorModePremultipliedArgb << kCursorModeShift) |
		(0u << kCursorPitchShift) | (3u << kCursorLinesPerChunkShift);
	regWriteDmu(2, kCursorControl + hubp, hubpControl | (hwCursorVisible ? 1u : 0u));
	regWriteDmu(2, kCursorCmScaleGY + dpp, kCursorFp16One);
	regWriteDmu(2, kCursorCmScaleRB + dpp, kCursorFp16One);
	regWriteDmu(2, kCursorCmMatrix + dpp, 0);
	regWriteDmu(2, kCursorCmControl + dpp,
	            kCursorCmWorkingBits | (kCursorModePremultipliedArgb << kCursorCmModeShift) |
	            (hwCursorVisible ? 1u : 0u));
	hwCursorSet = true;
	FBLOG("cursor: cscSetHardwareCursor image %ux%u hotspot %u,%u, addr 0x%llx ctl 0x%08x cm 0x%08x",
	      cursorWidth, cursorHeight, hwCursorHotX, hwCursorHotY, cursorMcAddr,
	      regReadDmu(2, kCursorControl + hubp), regReadDmu(2, kCursorCmControl + dpp));
	return kIOReturnSuccess;
}

IOReturn RDNA4Device::drawHardwareCursor(int32_t x, int32_t y, uint32_t visible) {
	if (!hwCursorReady || !hwCursorSet)
		return kIOReturnUnsupported;
	hwCursorX = x;
	hwCursorY = y;
	hwCursorVisible = visible != 0;
	ensureUpdateLatch();
	const uint32_t hubp = hubpOff();
	const uint32_t dpp = dppOff();
	const uint32_t px = static_cast<uint32_t>(x) & 0x7fff;
	const uint32_t py = static_cast<uint32_t>(y) & 0x7fff;
	regWriteDmu(2, kCursorPosition + hubp, py | (px << 15));
	regWriteDmu(2, kCursorDstOffset + hubp, x > 0 ? static_cast<uint32_t>(x) : 0);
	const uint32_t control = kCursorReqModePrefetch |
		(kCursorModePremultipliedArgb << kCursorModeShift) | (3u << kCursorLinesPerChunkShift);
	regWriteDmu(2, kCursorControl + hubp, control | (hwCursorVisible ? 1u : 0u));
	regWriteDmu(2, kCursorCmControl + dpp,
	            kCursorCmWorkingBits | (kCursorModePremultipliedArgb << kCursorCmModeShift) |
	            (hwCursorVisible ? 1u : 0u));
	FBLOG("cursor: cscDrawHardwareCursor x=%d y=%d visible=%u pos=0x%08x",
	      x, y, visible, regReadDmu(2, kCursorPosition + hubp));
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
