//
//  ndrv.hpp
//  RDNA4FB
//
//  Answers Apple's IONDRVFramebuffer the way a display NDRV would. On a GPU
//  without a macOS driver, IONDRVFramebuffer drives the GOP framebuffer
//  through IOBootNDRV, which knows one fixed mode (ID 100) and nothing about
//  the monitor. The plugin (plugin.cpp) hands the csc Status/Control
//  requests for our framebuffer to a Translator first; it answers the mode
//  list, timings, EDID, connection and DPMS requests from the RDNA4FB mode
//  table and the sink's EDID, and everything it does not handle (gamma,
//  CLUT, power state, cursor, ...) stays with IOBootNDRV.
//
//  Mode IDs: the boot mode keeps IOBootNDRV's 100 (IONDRVFramebuffer may
//  have cached it before the first request reached us); every other table
//  entry gets 100 + its Modes::Mode::id. They stay below 0x80000000, which
//  Apple reserves for programmable (detailed-timing) modes. There is one
//  depth, kDepthMode1 = 32 bpp direct.
//
//  The csc records are mirrored locally (the SDK's IOMacOSVideo.h needs the
//  kernel headers). IOMacOSVideo.h selects mac68k alignment only for 32-bit
//  builds; on x86_64 the records are naturally aligned, and plugin.cpp
//  static_asserts every mirror against the SDK type. Freestanding: shared by
//  the kext and the host test harness (LP64 hosts only).
//

#ifndef Ndrv_hpp
#define Ndrv_hpp

#include <stddef.h>
#include <stdint.h>

#include "modes.hpp"

namespace Ndrv {

// csc selectors (IOMacOSVideo.h).
enum : uint16_t {
	// Status
	cscGetCurMode         = 10,
	cscGetSync            = 11,
	cscGetConnection      = 12,
	cscGetModeTiming      = 13,
	cscGetNextResolution  = 17,
	cscGetVideoParameters = 18,
	cscGetDDCBlock        = 27,
	cscGetDetailedTiming  = 31,
	cscSupportsHardwareCursor = 22,
	cscGetHardwareCursorDrawState = 23,
	// Control
	cscSwitchMode         = 10,
	cscSetSync            = 11,
	cscSetHardwareCursor  = 22,
	cscDrawHardwareCursor = 23,
};

// IOReturn values used here.
enum : int32_t {
	kSuccess     = 0,
	kBadArgument = static_cast<int32_t>(0xe00002c2),
	kUnsupported = static_cast<int32_t>(0xe00002c7),
};

constexpr int32_t  kBootModeId                = 100;   // IOBootNDRV's kIOBootNDRVDisplayMode
constexpr int32_t  kDisplayModeIDCurrent      = 0;
constexpr int32_t  kDisplayModeIDInvalid      = -1;    // 0xFFFFFFFF
constexpr int32_t  kDisplayModeIDFindFirst    = -2;    // kDisplayModeIDFindFirstResolution
constexpr int32_t  kDisplayModeIDNoMore       = -3;    // kDisplayModeIDNoMoreResolutions
constexpr uint16_t kDepthMode1                = 128;
constexpr uint32_t kDeclROMtables             = 0x6465636c;   // 'decl'
constexpr uint32_t kTimingInvalid             = 0;
constexpr uint32_t kDisplayModeValidFlag      = 0x1;
constexpr uint32_t kDisplayModeSafeFlag       = 0x2;
constexpr uint32_t kDisplayModeDefaultFlag    = 0x4;
constexpr int16_t  kRGBDirectPixels           = 2;     // kIORGBDirectPixels
constexpr uint16_t kGenericLCD                = 20;
constexpr uint32_t kHasDirectConnection       = 1u << 3;
constexpr uint32_t kReportsDDCConnection      = 1u << 7;
constexpr uint32_t kHasDDCConnection          = 1u << 8;
constexpr uint32_t kDDCBlockTypeEDID          = 0;
constexpr uint32_t kSyncPositivePolarityMask  = 1;
constexpr uint32_t kDMSModeReady              = 0;
constexpr uint8_t  kDPMSSyncOn                = 0;
constexpr uint8_t  kDPMSSyncOff               = 7;
constexpr uint8_t  kDPMSSyncMask              = 7;     // H, V, composite sync
constexpr uint8_t  kSyncDisableHV             = 3;     // kHorizontalSyncMask | kVerticalSyncMask

// --- csc records (LP64 layout) ----------------------------------------------
static_assert(sizeof(void *) == 8, "csc record mirrors assume LP64");

struct Rect {
	int16_t top, left, bottom, right;
};

struct VPBlock {
	uint32_t vpBaseOffset;
	uint32_t vpRowBytes;          // UInt32 on LP64
	Rect     vpBounds;
	int16_t  vpVersion;
	int16_t  vpPackType;
	uint32_t vpPackSize;
	uint32_t vpHRes;
	uint32_t vpVRes;
	int16_t  vpPixelType;
	int16_t  vpPixelSize;
	int16_t  vpCmpCount;
	int16_t  vpCmpSize;
	uint32_t vpPlaneBytes;
};

struct VDSwitchInfoRec {
	uint16_t  csMode;             // depth mode
	int32_t   csData;             // display mode ID
	uint16_t  csPage;
	uintptr_t csBaseAddr;         // 1 | physical address
	uintptr_t csReserved;
};

struct VDTimingInfoRec {
	int32_t   csTimingMode;
	uintptr_t csTimingReserved;
	uint32_t  csTimingFormat;
	uint32_t  csTimingData;
	uint32_t  csTimingFlags;
};

struct VDDisplayConnectInfoRec {
	uint16_t  csDisplayType;
	uint8_t   csConnectTaggedType;
	uint8_t   csConnectTaggedData;
	uint32_t  csConnectFlags;
	uintptr_t csDisplayComponent;
	uintptr_t csConnectReserved;
};

struct VDSyncInfoRec {
	uint8_t csMode;
	uint8_t csFlags;
};

struct VDResolutionInfoRec {
	int32_t   csPreviousDisplayModeID;
	int32_t   csDisplayModeID;
	uint32_t  csHorizontalPixels;
	uint32_t  csVerticalLines;
	uint32_t  csRefreshRate;      // Fixed 16.16 Hz
	uint16_t  csMaxDepthMode;
	uint32_t  csResolutionFlags;
	uintptr_t csReserved;
};

struct VDVideoParametersInfoRec {
	int32_t   csDisplayModeID;
	uint16_t  csDepthMode;
	VPBlock  *csVPBlockPtr;
	uint32_t  csPageCount;
	uint32_t  csDeviceType;
	uint32_t  csDepthFlags;
};

struct VDDDCBlockRec {
	uint32_t ddcBlockNumber;      // 1-based
	uint32_t ddcBlockType;
	uint32_t ddcFlags;
	uint32_t ddcReserved;
	uint8_t  ddcBlockData[128];
};

struct VDDetailedTimingRec {
	uint32_t csTimingSize;
	uint32_t csTimingType;
	uint32_t csTimingVersion;
	uint32_t csTimingReserved;
	int32_t  csDisplayModeID;
	uint32_t csDisplayModeSeed;
	uint32_t csDisplayModeState;
	uint32_t csDisplayModeAlias;
	uint32_t csSignalConfig;
	uint32_t csSignalLevels;
	uint64_t csPixelClock;        // Hz
	uint64_t csMinPixelClock;
	uint64_t csMaxPixelClock;
	uint32_t csHorizontalActive;
	uint32_t csHorizontalBlanking;
	uint32_t csHorizontalSyncOffset;
	uint32_t csHorizontalSyncPulseWidth;
	uint32_t csVerticalActive;
	uint32_t csVerticalBlanking;
	uint32_t csVerticalSyncOffset;
	uint32_t csVerticalSyncPulseWidth;
	uint32_t csHorizontalBorderLeft;
	uint32_t csHorizontalBorderRight;
	uint32_t csVerticalBorderTop;
	uint32_t csVerticalBorderBottom;
	uint32_t csHorizontalSyncConfig;
	uint32_t csHorizontalSyncLevel;
	uint32_t csVerticalSyncConfig;
	uint32_t csVerticalSyncLevel;
	uint32_t csNumLinks;
	uint32_t csReserved2, csReserved3, csReserved4;
	uint32_t csReserved5, csReserved6, csReserved7, csReserved8;
};

// IOMacOSVideo.h cursor csc records (Apple IOGraphics).
struct VDSetHardwareCursorRec {
	void    *csCursorRef;
	uint32_t csReserved1;
	uint32_t csReserved2;
};

struct VDDrawHardwareCursorRec {
	int32_t  csCursorX;
	int32_t  csCursorY;
	uint32_t csCursorVisible;
	uint32_t csReserved1;
	uint32_t csReserved2;
};

struct VDSupportsHardwareCursorRec {
	uint32_t csSupportsHardwareCursor;
	uint32_t csReserved1;
	uint32_t csReserved2;
};

struct VDHardwareCursorDrawStateRec {
	int32_t  csCursorX;
	int32_t  csCursorY;
	uint32_t csCursorVisible;
	uint32_t csCursorSet;
	uint32_t csReserved1;
	uint32_t csReserved2;
};


// --- backend -----------------------------------------------------------------

// The memory macOS draws into for one mode.
struct Surface {
	uint64_t physBase;
	uint32_t rowBytes;
	uint32_t width, height;
};

// What the hardware side provides. `isBoot` marks the mode the GOP lit.
struct Backend {
	void *ctx;
	// Surface for `m`; false = the mode cannot be displayed.
	bool (*surfaceFor)(void *ctx, const Modes::Mode &m, bool isBoot, Surface &out);
	// Program `m` (an IOReturn value; the current mode stays on failure).
	int32_t (*switchTo)(void *ctx, const Modes::Mode &m, bool isBoot);
	// DPMS: display on or blanked.
	void (*setPower)(void *ctx, bool on);
	bool (*supportsHardwareCursor)(void *ctx);
	int32_t (*setHardwareCursor)(void *ctx, void *cursorRef);
	int32_t (*drawHardwareCursor)(void *ctx, int32_t x, int32_t y, uint32_t visible);
	int32_t (*getHardwareCursorDrawState)(void *ctx, VDHardwareCursorDrawStateRec &state);
};

class Translator {
public:
	// `modes` (count entries, bootModeId = Modes::Mode::id of the live
	// mode) and `edid` must outlive the Translator. count 0 leaves every
	// mode request to IOBootNDRV; edidLen 0 leaves the DDC request to it.
	void init(const Modes::Mode *modes, size_t count, uint32_t bootModeId,
	          const uint8_t *edid, size_t edidLen, const Backend &backend);

	// Handle one csc Status/Control request. Returns false if the request
	// is not ours to answer (the caller then passes it on to IOBootNDRV);
	// otherwise `ret` holds the IOReturn for IONDRVFramebuffer.
	bool status(uint16_t code, void *params, int32_t &ret);
	bool control(uint16_t code, void *params, int32_t &ret);

	int32_t currentModeId() const { return current; }
	size_t  modeCount() const { return count; }
	// NDRV mode ID of table entry `m`.
	int32_t idFor(const Modes::Mode &m) const;

private:
	const Modes::Mode *modes { nullptr };
	size_t   count { 0 };
	uint32_t bootModeId { 0 };
	const uint8_t *edid { nullptr };
	size_t   edidLen { 0 };
	Backend  be {};
	int32_t  current { kBootModeId };
	uint8_t  syncState { kDPMSSyncOn };

	const Modes::Mode *modeFor(int32_t id) const;
	bool surface(const Modes::Mode &m, Surface &out) const;

	int32_t getCurMode(VDSwitchInfoRec &r) const;
	int32_t getNextResolution(VDResolutionInfoRec &r) const;
	int32_t getVideoParameters(VDVideoParametersInfoRec &r) const;
	int32_t getModeTiming(VDTimingInfoRec &r) const;
	int32_t getDetailedTiming(VDDetailedTimingRec &r) const;
	int32_t getConnection(VDDisplayConnectInfoRec &r) const;
	int32_t getDDCBlock(VDDDCBlockRec &r) const;
	int32_t getSync(VDSyncInfoRec &r) const;
	int32_t switchMode(VDSwitchInfoRec &r);
	int32_t setSync(const VDSyncInfoRec &r);
};

} // namespace Ndrv

// Opaque bridges to IONDRVFramebuffer's VSL services. The service is made
// by Apple's VSLNewInterruptService; W1 only invokes VSLDoInterruptService
// from its deferred IH work-loop action.
namespace Ndrv {
using VslDoInterruptService = int32_t (*)(void *service);
using VslPrepareCursor = bool (*)(void *cursorRef, void *descriptor, void *info);
void vslInit();
void vslServiceCreated(void *service, VslDoInterruptService doService);
bool vslServiceDisposed(void *service);
bool vslServicePresent();
void vslPrepareCursorInstalled(VslPrepareCursor prepare);
void setVblankEnabled(bool enabled);
void signalVblank();
bool prepareCursor(void *cursorRef, void *descriptor, void *info);
}

#endif /* Ndrv_hpp */
