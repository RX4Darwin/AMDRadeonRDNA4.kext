//
//  ndrv.cpp
//  RDNA4FB
//
//  See ndrv.hpp. The replies follow IOBootNDRV (IONDRVFramebuffer.cpp,
//  IOGraphics) field for field where it answers the same request, so Apple's
//  side sees the same record shapes it already handles for mode 100.
//

#include "ndrv.hpp"

#ifdef KERNEL
#include <IOKit/IOLocks.h>
#endif

namespace Ndrv {

namespace {
void *gVblService { nullptr };
VslDoInterruptService gVblDo { nullptr };
VslPrepareCursor gPrepareCursor { nullptr };
bool gVblankEnabled { false };
#ifdef KERNEL
IOLock *gVblLock { nullptr };
#else
bool gVblLock { false };
#endif

void vslLock() {
#ifdef KERNEL
	IOLockLock(gVblLock);
#endif
}

void vslUnlock() {
#ifdef KERNEL
	IOLockUnlock(gVblLock);
#endif
}
}

void vslInit() {
#ifdef KERNEL
	if (!gVblLock)
		gVblLock = IOLockAlloc();
#else
	gVblLock = true;
#endif
}

void vslServiceCreated(void *service, VslDoInterruptService doService) {
	if (!gVblLock)
		vslInit();
	if (!gVblLock || !service || !doService)
		return;
	vslLock();
	if (!gVblService) {
		gVblService = service;
		gVblDo = doService;
	}
	vslUnlock();
}

bool vslServiceDisposed(void *service) {
	if (!gVblLock)
		return false;
	vslLock();
	const bool owned = service && service == gVblService;
	if (owned) {
		gVblService = nullptr;
		gVblDo = nullptr;
		gVblankEnabled = false;
	}
	vslUnlock();
	return owned;
}

bool vslServicePresent() {
	if (!gVblLock)
		return false;
	vslLock();
	const bool present = gVblService != nullptr;
	vslUnlock();
	return present;
}

void vslPrepareCursorInstalled(VslPrepareCursor prepare) {
	gPrepareCursor = prepare;
}

void setVblankEnabled(bool enabled) {
	if (!gVblLock)
		vslInit();
	if (!gVblLock)
		return;
	vslLock();
	gVblankEnabled = enabled;
	vslUnlock();
}

void signalVblank() {
	if (!gVblLock)
		return;
	vslLock();
	if (gVblankEnabled && gVblService && gVblDo)
		(void)gVblDo(gVblService);
	vslUnlock();
}

bool prepareCursor(void *cursorRef, void *descriptor, void *info) {
	return gPrepareCursor && gPrepareCursor(cursorRef, descriptor, info);
}

void Translator::init(const Modes::Mode *table, size_t n, uint32_t bootId,
                      const uint8_t *edidBytes, size_t edidBytesLen, const Backend &backend) {
	modes = table;
	count = table ? n : 0;
	bootModeId = bootId;
	edid = edidBytes;
	edidLen = edidBytes ? (edidBytesLen / 128) * 128 : 0;
	be = backend;
	current = kBootModeId;
	syncState = kDPMSSyncOn;
	// Without the boot entry the IDs could not be tied to what IOBootNDRV
	// already reported, so the mode requests stay with it.
	bool haveBoot = false;
	for (size_t i = 0; i < count; i++)
		haveBoot |= modes[i].id == bootModeId;
	if (!haveBoot)
		count = 0;
}

int32_t Translator::idFor(const Modes::Mode &m) const {
	return m.id == bootModeId ? kBootModeId : kBootModeId + static_cast<int32_t>(m.id);
}

const Modes::Mode *Translator::modeFor(int32_t id) const {
	for (size_t i = 0; i < count; i++)
		if (idFor(modes[i]) == id)
			return &modes[i];
	return nullptr;
}

bool Translator::surface(const Modes::Mode &m, Surface &out) const {
	return be.surfaceFor && be.surfaceFor(be.ctx, m, m.id == bootModeId, out) &&
	       out.width && out.height && out.rowBytes >= out.width * 4u;
}

bool Translator::status(uint16_t code, void *params, int32_t &ret) {
	if (!params)
		return false;
	const bool haveModes = count != 0;
	switch (code) {
	case cscGetCurMode:
		if (!haveModes)
			return false;
		ret = getCurMode(*static_cast<VDSwitchInfoRec *>(params));
		return true;
	case cscGetNextResolution:
		if (!haveModes)
			return false;
		ret = getNextResolution(*static_cast<VDResolutionInfoRec *>(params));
		return true;
	case cscGetVideoParameters:
		if (!haveModes)
			return false;
		ret = getVideoParameters(*static_cast<VDVideoParametersInfoRec *>(params));
		return true;
	case cscGetModeTiming:
		if (!haveModes)
			return false;
		ret = getModeTiming(*static_cast<VDTimingInfoRec *>(params));
		return true;
	case cscGetDetailedTiming:
		if (!haveModes)
			return false;
		ret = getDetailedTiming(*static_cast<VDDetailedTimingRec *>(params));
		return true;
	case cscGetConnection:
		ret = getConnection(*static_cast<VDDisplayConnectInfoRec *>(params));
		return true;
	case cscGetDDCBlock:
		if (!edidLen)
			return false;
		ret = getDDCBlock(*static_cast<VDDDCBlockRec *>(params));
		return true;
	case cscGetSync:
		if (!be.setPower)
			return false;
		ret = getSync(*static_cast<VDSyncInfoRec *>(params));
		return true;
	case cscSupportsHardwareCursor: {
		if (!be.supportsHardwareCursor)
			return false;
		// With rdna4-cursor absent the device deliberately remains a software
		// cursor.  Let IONDRV's original implementation answer both cursor
		// status selectors instead of changing its contract to "unsupported".
		if (!be.supportsHardwareCursor(be.ctx))
			return false;
		auto &r = *static_cast<VDSupportsHardwareCursorRec *>(params);
		r.csSupportsHardwareCursor = 1u;
		r.csReserved1 = 0;
		r.csReserved2 = 0;
		ret = kSuccess;
		return true;
	}
	case cscGetHardwareCursorDrawState:
		if (!be.getHardwareCursorDrawState || !be.supportsHardwareCursor ||
		    !be.supportsHardwareCursor(be.ctx))
			return false;
		ret = be.getHardwareCursorDrawState(
			be.ctx, *static_cast<VDHardwareCursorDrawStateRec *>(params));
		return true;
	default:
		return false;
	}
}

bool Translator::control(uint16_t code, void *params, int32_t &ret) {
	if (!params)
		return false;
	switch (code) {
	case cscSwitchMode:
		if (!count)
			return false;
		ret = switchMode(*static_cast<VDSwitchInfoRec *>(params));
		return true;
	case cscSetSync:
		if (!be.setPower)
			return false;
		ret = setSync(*static_cast<const VDSyncInfoRec *>(params));
		return true;
	case cscSetHardwareCursor: {
		if (!be.setHardwareCursor)
			return false;
		auto &r = *static_cast<VDSetHardwareCursorRec *>(params);
		ret = be.setHardwareCursor(be.ctx, r.csCursorRef);
		return true;
	}
	case cscDrawHardwareCursor: {
		if (!be.drawHardwareCursor)
			return false;
		auto &r = *static_cast<VDDrawHardwareCursorRec *>(params);
		ret = be.drawHardwareCursor(be.ctx, r.csCursorX, r.csCursorY,
		                            r.csCursorVisible);
		return true;
	}
	default:
		return false;
	}
}

// --- Status ------------------------------------------------------------------

int32_t Translator::getCurMode(VDSwitchInfoRec &r) const {
	const Modes::Mode *m = modeFor(current);
	Surface s {};
	if (!m || !surface(*m, s))
		return kUnsupported;
	r.csData = current;
	r.csMode = kDepthMode1;
	r.csPage = 1;
	r.csBaseAddr = static_cast<uintptr_t>(1 | s.physBase);   // physical, as IOBootNDRV
	return kSuccess;
}

int32_t Translator::getNextResolution(VDResolutionInfoRec &r) const {
	const int32_t prev = r.csPreviousDisplayModeID;
	const Modes::Mode *m = nullptr;
	if (prev == kDisplayModeIDFindFirst) {
		m = &modes[0];
	} else if (prev == kDisplayModeIDCurrent) {
		m = modeFor(current);
	} else {
		size_t i = 0;
		while (i < count && idFor(modes[i]) != prev)
			i++;
		if (i == count) {
			// Unknown ID, or kDisplayModeIDFindFirstProgrammable: there
			// are no programmable timings.
			r.csDisplayModeID = kDisplayModeIDInvalid;
			return kBadArgument;
		}
		if (i + 1 == count) {
			r.csDisplayModeID = kDisplayModeIDNoMore;
			return kSuccess;
		}
		m = &modes[i + 1];
	}
	if (!m) {
		r.csDisplayModeID = kDisplayModeIDInvalid;
		return kBadArgument;
	}
	r.csDisplayModeID    = idFor(*m);
	r.csHorizontalPixels = m->t.hActive;
	r.csVerticalLines    = m->t.vActive;
	r.csRefreshRate      = Modes::refresh1616(m->t);
	r.csMaxDepthMode     = kDepthMode1;
	r.csResolutionFlags  = 0;
	return kSuccess;
}

int32_t Translator::getVideoParameters(VDVideoParametersInfoRec &r) const {
	const Modes::Mode *m = modeFor(r.csDisplayModeID);
	Surface s {};
	if (!m || r.csDepthMode != kDepthMode1 || !r.csVPBlockPtr || !surface(*m, s))
		return kBadArgument;
	VPBlock &p = *r.csVPBlockPtr;
	p = VPBlock {};
	p.vpBounds.right  = static_cast<int16_t>(s.width);
	p.vpBounds.bottom = static_cast<int16_t>(s.height);
	p.vpRowBytes      = s.rowBytes;
	p.vpPixelSize     = 32;
	p.vpPixelType     = kRGBDirectPixels;
	p.vpCmpCount      = 3;
	p.vpCmpSize       = 8;
	return kSuccess;
}

int32_t Translator::getModeTiming(VDTimingInfoRec &r) const {
	const Modes::Mode *m = modeFor(r.csTimingMode);
	if (!m)
		return kBadArgument;
	// kDeclROMtables with an unknown Apple timing: IONDRVFramebuffer then
	// passes csTimingFlags through (kDetailedTimingFormat would replace
	// them with Valid|Safe and lose the default flag). The exact raster
	// comes from cscGetDetailedTiming, which it asks for independently.
	r.csTimingFormat = kDeclROMtables;
	r.csTimingData   = kTimingInvalid;
	// Every mode is valid and safe. A mode without the safe flag is not
	// switched to on macOS 26: WindowServer offers it as a scaled rendering
	// of the default mode instead ("Using a scaled resolution..."), so the
	// driver would never see the switch.
	r.csTimingFlags = kDisplayModeValidFlag | kDisplayModeSafeFlag;
	if (m->id == bootModeId)
		r.csTimingFlags |= kDisplayModeDefaultFlag;
	return kSuccess;
}

int32_t Translator::getDetailedTiming(VDDetailedTimingRec &r) const {
	const Modes::Mode *m = modeFor(r.csDisplayModeID);
	if (!m || (r.csTimingSize && r.csTimingSize < sizeof(VDDetailedTimingRec)))
		return kBadArgument;
	const Edid::DetailedTiming &t = m->t;
	const int32_t id = r.csDisplayModeID;
	r = VDDetailedTimingRec {};
	r.csTimingSize               = sizeof(VDDetailedTimingRec);
	r.csDisplayModeID            = id;
	r.csDisplayModeState         = kDMSModeReady;
	r.csDisplayModeAlias         = static_cast<uint32_t>(id);
	r.csPixelClock               = static_cast<uint64_t>(t.pixelClockKHz) * 1000;
	r.csMinPixelClock            = r.csPixelClock;
	r.csMaxPixelClock            = r.csPixelClock;
	r.csHorizontalActive         = t.hActive;
	r.csHorizontalBlanking       = t.hBlank;
	r.csHorizontalSyncOffset     = t.hSyncOffset;
	r.csHorizontalSyncPulseWidth = t.hSyncWidth;
	r.csVerticalActive           = t.vActive;
	r.csVerticalBlanking         = t.vBlank;
	r.csVerticalSyncOffset       = t.vSyncOffset;
	r.csVerticalSyncPulseWidth   = t.vSyncWidth;
	r.csHorizontalSyncConfig     = t.hSyncPositive ? kSyncPositivePolarityMask : 0;
	r.csVerticalSyncConfig       = t.vSyncPositive ? kSyncPositivePolarityMask : 0;
	return kSuccess;
}

int32_t Translator::getConnection(VDDisplayConnectInfoRec &r) const {
	r.csDisplayType       = kGenericLCD;
	r.csConnectTaggedType = 0;
	r.csConnectTaggedData = 0;
	// IONDRVFramebuffer::hasDDCConnect needs both DDC bits.
	r.csConnectFlags      = kReportsDDCConnection;
	if (edidLen)
		r.csConnectFlags |= kHasDDCConnection | kHasDirectConnection;
	r.csDisplayComponent  = 0;
	r.csConnectReserved   = 0;
	return kSuccess;
}

int32_t Translator::getDDCBlock(VDDDCBlockRec &r) const {
	const size_t blocks = edidLen / 128;
	if (r.ddcBlockType != kDDCBlockTypeEDID || r.ddcBlockNumber < 1 || r.ddcBlockNumber > blocks)
		return kBadArgument;
	const uint8_t *src = edid + (r.ddcBlockNumber - 1) * 128;
	for (size_t i = 0; i < 128; i++)
		r.ddcBlockData[i] = src[i];
	if (r.ddcBlockNumber == 1 && r.ddcBlockData[126] > blocks - 1) {
		// More extensions than were read: announce only the cached ones
		// so the reader does not ask for blocks we cannot serve.
		r.ddcBlockData[126] = static_cast<uint8_t>(blocks - 1);
		uint8_t sum = 0;
		for (size_t i = 0; i < 127; i++)
			sum = static_cast<uint8_t>(sum + r.ddcBlockData[i]);
		r.ddcBlockData[127] = static_cast<uint8_t>(0x100 - sum);
	}
	return kSuccess;
}

int32_t Translator::getSync(VDSyncInfoRec &r) const {
	// csMode 0xFF asks which sync lines can be controlled; anything else
	// asks for their current state.
	r.csMode  = r.csMode == 0xFF ? kDPMSSyncMask : syncState;
	r.csFlags = 0;
	return kSuccess;
}

// --- Control -----------------------------------------------------------------

int32_t Translator::switchMode(VDSwitchInfoRec &r) {
	const Modes::Mode *m = modeFor(r.csData);
	Surface s {};
	if (!m || r.csMode != kDepthMode1 || !surface(*m, s))
		return kBadArgument;
	if (r.csData != current) {
		int32_t err = be.switchTo ? be.switchTo(be.ctx, *m, m->id == bootModeId) : kUnsupported;
		if (err != kSuccess)
			return err;
		current = r.csData;
		// The backend may place the new mode's surface elsewhere.
		if (!surface(*m, s))
			return kBadArgument;
	}
	r.csBaseAddr = static_cast<uintptr_t>(1 | s.physBase);
	return kSuccess;
}

int32_t Translator::setSync(const VDSyncInfoRec &r) {
	// csFlags selects which csMode bits apply (IONDRVFramebuffer sends
	// kDPMSSyncMask); standby and suspend blank like off.
	const uint8_t mask = r.csFlags ? r.csFlags : kDPMSSyncMask;
	const uint8_t next = static_cast<uint8_t>(((syncState & ~mask) | (r.csMode & mask)) & kDPMSSyncMask);
	const bool wasOn = (syncState & kSyncDisableHV) == 0;
	const bool on    = (next & kSyncDisableHV) == 0;
	syncState = next;
	if (on != wasOn)
		be.setPower(be.ctx, on);
	return kSuccess;
}

// --- a head with no IOBootNDRV behind it -------------------------------------

int32_t bootReply(bool isStatus, uint16_t code) {
	return !isStatus && (code == cscSetEntries || code == cscSetGamma) ? kSuccess : kUnsupported;
}

bool spareSurface(uint64_t fbBase, uint64_t fbLen, uint64_t rangeBase, uint64_t rangeLen,
                  uint32_t width, uint32_t height, Surface &out) {
	constexpr uint64_t kMiB = 1ull << 20;
	if (!width || !height || !fbLen || fbBase < rangeBase || fbLen > rangeLen ||
	    fbBase - rangeBase > rangeLen - fbLen)
		return false;
	const uint64_t start = (fbBase + fbLen + 2 * kMiB - 1) & ~(kMiB - 1);
	const uint64_t bytes = static_cast<uint64_t>(width) * 4 * height + 128;
	const uint64_t end = rangeBase + rangeLen;
	if (start > end || bytes > end - start)
		return false;
	out = { start, width * 4, width, height };
	return true;
}

} // namespace Ndrv
