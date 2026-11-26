//
//  ndrv.hpp
//  AMDRadeonRDNA4
//
//  Answers Apple's IONDRVFramebuffer the way a display NDRV would. On a GPU
//  without a macOS driver, IONDRVFramebuffer drives the GOP framebuffer
//  through IOBootNDRV, which knows one fixed mode (ID 100) and nothing about
//  the monitor. The plugin (plugin.cpp) hands the csc Status/Control
//  requests for our framebuffer to a Translator first; it answers the mode
//  list, timings, EDID, connection and DPMS requests from the AMDRadeonRDNA4 mode
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
	cscGetTimingRanges    = 30,
	cscGetDetailedTiming  = 31,
	cscSupportsHardwareCursor = 22,
	cscGetHardwareCursorDrawState = 23,
	// Control
	cscSetEntries         = 3,
	cscSetGamma           = 4,
	cscSwitchMode         = 10,
	cscSetSync            = 11,
	cscSetDetailedTiming  = 31,
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
constexpr int32_t  kDisplayModeIDFindFirstProgrammable = -4;
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
constexpr uint32_t kConnectionInactive        = 1u << 9;    // nothing connected: IONDRVFramebuffer goes offline
constexpr uint32_t kDDCBlockTypeEDID          = 0;
constexpr uint32_t kSyncPositivePolarityMask  = 1;
constexpr uint32_t kDMSModeReady              = 0;
constexpr uint32_t kDMSModeNotReady           = 1;
constexpr uint32_t kDMSModeFree               = 2;
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

// IONDRVFramebuffer publishes it as the property IOFBTimingRange (IODisplayTimingRange, the same layout).
struct VDDisplayTimingRangeRec {
	uint32_t csRangeSize, csRangeType, csRangeVersion, csRangeReserved;
	uint32_t csRangeBlockIndex, csRangeGroup, csRangeBlockCount, csRangeFlags;
	uint64_t csMinPixelClock, csMaxPixelClock;                        // Hz
	uint32_t csMaxPixelError;
	uint32_t csTimingRangeSyncFlags;                                  // kRangeSupports*
	uint32_t csTimingRangeSignalLevels, csTimingRangeSupportedSignalConfigs;
	uint32_t csMinFrameRate, csMaxFrameRate, csMinLineRate, csMaxLineRate;   // Hz
	uint32_t csMaxHorizontalTotal, csMaxVerticalTotal, csMaxTotalReserved1, csMaxTotalReserved2;
	uint8_t  csCharSize[14];                                          // h active .. v total
	uint16_t csCharSizeReserved1;
	uint32_t csMinMax[24];                                            // min/max h active .. v border bottom
	uint32_t csMaxNumLinks, csMinLink0PixelClock, csMaxLink0PixelClock, csMinLink1PixelClock, csMaxLink1PixelClock;
	uint16_t csSupportedPixelEncoding, csSupportedBitsPerColorComponent;      // IODisplayTimingRangeV2's names, kIORange*
	uint16_t csSupportedColorimetryModes, csSupportedDynamicRangeModes;
	uint32_t csReserved8;
};
// IOGraphicsTypes.h's supportedSyncFlags bits.
constexpr uint32_t kRangeSupportsSeparateSyncs = 0x01, kRangeSupportsVRR = 0x10;
// Its colour bits (kIORangePixelEncoding*, kIORangeBitsPerColorComponent*, kIORangeColorimetry*, kIORangeDynamicRange*),
// the same values a timing's single fields take (kIOPixelEncoding*, ...).
constexpr uint16_t kRangeEncodingRGB444 = 0x01, kRangeBpc8 = 0x02, kRangeBpc10 = 0x04, kRangeColorimetrySRGB = 0x02,
                   kRangeColorimetryBT2020 = 0x100, kRangeDynamicSDR = 0x01, kRangeDynamicHDR10 = 0x02;

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
	uint32_t csReserved2;         // IODetailedTimingInformationV2::verticalBlankingExtension (lines)
	uint16_t csPixelEncoding, csBitsPerColorComponent;   // IODetailedTimingInformationV2's colour fields (kIO*)
	uint16_t csColorimetry, csDynamicRange;
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
	// `m` (the mode on) with variable refresh down to `extLines` lines more than its V total, or fixed (0).
	int32_t (*setRefresh)(void *ctx, const Modes::Mode &m, uint32_t extLines);
	// Optional (hdr): after each switch, whether the mode is an HDR10 twin; the sink is told (HDR metadata) or untold.
	int32_t (*setHdr)(void *ctx, bool on);
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
	// rdna4-vrr=1 (docs/freesync-plan.md): answer cscGetTimingRanges, with variable refresh between the EDID's rates, and
	// take the timings macOS installs from the EDID (programmable slots); CoreDisplay offers variable refresh only on those.
	bool adaptiveSync { false };
	// rdna4-hdr=1 on a sink that takes PQ (docs/hdr-plan.md): the range (served only with adaptiveSync) also offers 10 bits
	// a component, BT.2020 and HDR10, and every table mode is listed a second time as HDR10 (its ID | kHdrIdBit):
	// CoreDisplay offers HDR only for a mode with such a twin. Switching to a twin calls Backend::setHdr.
	bool hdr { false };
	static constexpr int32_t kHdrIdBit = 0x20000000;
	static bool isHdrId(int32_t id) { return id > 0 && (id & kHdrIdBit) && !isSlotId(id); }
	// An HDR twin of a mode smaller than a 3840x2160-class monitor (Modes::scaleBase) is listed with the mode's timing (CoreDisplay
	// pairs the twins by timing) but runs as a scaled frame: the head on the base mode, macOS's frame of the mode's size scaled up
	// to it when presented, as AMD's Windows driver does. A monitor then only ever gets its native HDR signal (docs/hdr-plan.md).
	bool control(uint16_t code, void *params, int32_t &ret);

	int32_t currentModeId() const { return current; }

	// Whether a display is on the connector. Without one cscGetConnection
	// says so and there is no DDC block: macOS takes the framebuffer offline
	// until a connect interrupt makes it ask again.
	void setConnected(bool on) { connected = on; }
	size_t  modeCount() const { return count; }
	// Frames above the head's modes (Modes::buildScaled), listed after them. `scaled` must outlive the Translator; init clears them.
	void setScaled(const Modes::Mode *scaled, size_t n);
	// The frame size of the scaled mode macOS is in; false in a mode the head runs itself.
	bool scaledFrame(uint32_t &width, uint32_t &height) const;
	// NDRV mode ID of table entry `m`.
	int32_t idFor(const Modes::Mode &m) const;

private:
	const Modes::Mode *modes { nullptr };
	size_t   count { 0 };
	const Modes::Mode *scaledModes { nullptr };
	size_t   scaledCount { 0 };
	uint32_t bootModeId { 0 };
	const uint8_t *edid { nullptr };
	size_t   edidLen { 0 };
	Backend  be {};
	int32_t  current { kBootModeId };
	uint8_t  syncState { kDPMSSyncOn };
	bool     connected { true };
	// Programmable timings (cscSetDetailedTiming): each holds a timing macOS installed under an arbitrary mode ID (the
	// alias), accepted only if it is one of the table's modes, which it then stands for.
	struct Slot {
		VDDetailedTimingRec timing;
		const Modes::Mode  *mode;
	};
	static constexpr size_t  kSlots = 8;
	static constexpr int32_t kSlotIdBase = 0x40000000;   // the slots' own IDs, apart from the table's
	Slot     slots[kSlots] {};
	uint32_t slotSeed { 0 };
	bool     variable { false };   // the backend runs the mode on with variable refresh
	static bool isSlotId(int32_t id);
	Slot *slotFor(int32_t id);
	const Slot *slotFor(int32_t id) const { return const_cast<Translator *>(this)->slotFor(id); }
	const Modes::Mode *tableMatch(const VDDetailedTimingRec &t) const;
	int32_t setDetailedTiming(const VDDetailedTimingRec &r);

	size_t total() const { return count + __atomic_load_n(&scaledCount, __ATOMIC_ACQUIRE); }
	size_t entries() const { return total() + (hdr ? count : 0); }   // what macOS lists: the modes, then their HDR10 twins
	int32_t entryId(size_t i) const { return i < total() ? idFor(at(i)) : idFor(modes[i - total()]) | kHdrIdBit; }
	const Modes::Mode &entry(size_t i) const { return i < total() ? at(i) : modes[i - total()]; }
	const Modes::Mode &at(size_t i) const { return i < count ? modes[i] : scaledModes[i - count]; }
	const Modes::Mode *modeFor(int32_t id) const;   // the mode as listed
	const Modes::Mode *runFor(int32_t id) const;    // as it runs: an HDR twin below the base mode as a scaled frame
	mutable Modes::Mode hdrFrames[Modes::MaxModes] {};
	bool surface(const Modes::Mode &m, Surface &out) const;

	int32_t getCurMode(VDSwitchInfoRec &r) const;
	int32_t getNextResolution(VDResolutionInfoRec &r) const;
	int32_t getVideoParameters(VDVideoParametersInfoRec &r) const;
	int32_t getModeTiming(VDTimingInfoRec &r) const;
	int32_t getDetailedTiming(VDDetailedTimingRec &r) const;
	int32_t getTimingRanges(VDDisplayTimingRangeRec &r) const;
	int32_t getConnection(VDDisplayConnectInfoRec &r) const;
	int32_t getDDCBlock(VDDDCBlockRec &r) const;
	int32_t getSync(VDSyncInfoRec &r) const;
	int32_t switchMode(VDSwitchInfoRec &r);
	int32_t setSync(const VDSyncInfoRec &r);
};

// --- a head with no IOBootNDRV behind it (docs/second-head-ndrv.md) ----------

// What IOBootNDRV answers to a csc request the Translator leaves alone: the
// CLUT and gamma writes succeed and do nothing, everything else is
// unsupported (IOBootNDRV::doControl / doStatus).
int32_t bootReply(bool isStatus, uint16_t code);

// The primary plane must fit in full. Secondary slots may grow up to the start of compute.
constexpr uint64_t displayReservation(uint64_t primary, uint64_t slots, uint64_t limit) {
	if (primary > limit)
		return 0;
	return slots > limit - primary ? limit : primary + slots;
}

// Allow room for a future mode switch without exposing the compute window past the display range.
constexpr uint64_t headMappingLength(uint64_t offset, uint64_t limit, uint64_t bytes, uint64_t preferred) {
	if (offset >= limit || !bytes || bytes > limit - offset)
		return 0;
	const uint64_t requested = bytes > preferred ? bytes : preferred;
	return requested < limit - offset ? requested : limit - offset;
}

// Place a width x height, 32 bpp surface for a second head in the memory
// range [rangeBase, rangeBase + rangeLen) that holds the console framebuffer
// [fbBase, fbBase + fbLen): 1 MiB aligned and at least 1 MiB past the
// console's end (the cursor sprite sits right behind it), with the 128 bytes
// IONDRVFramebuffer::getApertureRange adds. False if it does not fit.
bool spareSurface(uint64_t fbBase, uint64_t fbLen, uint64_t rangeBase, uint64_t rangeLen,
                  uint32_t width, uint32_t height, Surface &out);

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
