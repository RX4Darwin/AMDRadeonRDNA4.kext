//
//  ndrv.cpp
//  AMDRadeonRDNA4
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
	__atomic_store_n(&scaledCount, 0, __ATOMIC_RELEASE);
	scaledModes = nullptr;
	bootModeId = bootId;
	edid = edidBytes;
	edidLen = edidBytes ? (edidBytesLen / 128) * 128 : 0;
	// Keep the original DDC reply for short EDIDs. Advertise additional blocks
	// only up to the first bad checksum; block indices must stay contiguous.
	if (edidLen > 256) {
		size_t valid = 128;
		while (valid < edidLen && valid / 128 <= edid[126] &&
		       Edid::blockChecksumOk(edid + valid))
			valid += 128;
		edidLen = valid;
	}
	be = backend;
	current = kBootModeId;
	syncState = kDPMSSyncOn;
	for (Slot &sl : slots)
		sl = Slot {};
	variable = false;
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
	if (hdr && isHdrId(id)) {
		for (size_t i = 0; i < count; i++)
			if ((idFor(modes[i]) | kHdrIdBit) == id)
				return &modes[i];
		return nullptr;
	}
	const size_t n = total();
	for (size_t i = 0; i < n; i++)
		if (idFor(at(i)) == id)
			return &at(i);
	const Slot *sl = slotFor(id);
	return sl ? sl->mode : nullptr;
}

bool Translator::isSlotId(int32_t id) {
	return id >= kSlotIdBase && id < kSlotIdBase + static_cast<int32_t>(kSlots);
}

// A slot by its own ID, or the ready one holding `id` as its alias.
Translator::Slot *Translator::slotFor(int32_t id) {
	if (isSlotId(id))
		return &slots[id - kSlotIdBase];
	for (Slot &sl : slots)
		if (sl.mode && sl.timing.csDisplayModeState == kDMSModeReady &&
		    sl.timing.csDisplayModeAlias == static_cast<uint32_t>(id))
			return &sl;
	return nullptr;
}

// The table mode a timing from macOS is: the same raster, the clock within 0.1 %, progressive, no borders. Polarities and
// the variable-refresh extension do not take part (the table's own timing is what gets programmed).
const Modes::Mode *Translator::tableMatch(const VDDetailedTimingRec &t) const {
	constexpr uint32_t kInterlaced = 0x4;   // kIOInterlacedCEATiming
	if ((t.csSignalConfig & kInterlaced) || t.csHorizontalBorderLeft || t.csHorizontalBorderRight ||
	    t.csVerticalBorderTop || t.csVerticalBorderBottom)
		return nullptr;
	for (size_t i = 0; i < count; i++) {
		const Edid::DetailedTiming &m = modes[i].t;
		const uint64_t hz = static_cast<uint64_t>(m.pixelClockKHz) * 1000;
		const uint64_t diff = t.csPixelClock > hz ? t.csPixelClock - hz : hz - t.csPixelClock;
		if (t.csHorizontalActive == m.hActive && t.csHorizontalBlanking == m.hBlank &&
		    t.csHorizontalSyncOffset == m.hSyncOffset && t.csHorizontalSyncPulseWidth == m.hSyncWidth &&
		    t.csVerticalActive == m.vActive && t.csVerticalBlanking == m.vBlank &&
		    t.csVerticalSyncOffset == m.vSyncOffset && t.csVerticalSyncPulseWidth == m.vSyncWidth && diff * 1000 <= hz)
			return &modes[i];
	}
	return nullptr;
}

void Translator::setScaled(const Modes::Mode *scaled, size_t n) {
	__atomic_store_n(&scaledCount, 0, __ATOMIC_RELEASE);
	scaledModes = scaled;
	__atomic_store_n(&scaledCount, count && scaled ? n : 0, __ATOMIC_RELEASE);
}

bool Translator::scaledFrame(uint32_t &width, uint32_t &height) const {
	const Modes::Mode *m = modeFor(current);
	if (!m || m->source != Modes::SourceScaled)
		return false;
	width = m->t.hActive;
	height = m->t.vActive;
	return true;
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
	case cscGetTimingRanges:
		if (!haveModes || !adaptiveSync)
			return false;
		ret = getTimingRanges(*static_cast<VDDisplayTimingRangeRec *>(params));
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
		if (!edidLen || !connected)
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
	case cscSetDetailedTiming:
		if (!count || !adaptiveSync)
			return false;
		ret = setDetailedTiming(*static_cast<const VDDetailedTimingRec *>(params));
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
	if (adaptiveSync && (prev == kDisplayModeIDFindFirstProgrammable || isSlotId(prev))) {
		const int32_t next = prev == kDisplayModeIDFindFirstProgrammable ? kSlotIdBase : prev + 1;
		r.csDisplayModeID = isSlotId(next) ? next : kDisplayModeIDNoMore;
		return kSuccess;
	}
	const Modes::Mode *m = nullptr;
	int32_t id = 0;
	if (prev == kDisplayModeIDFindFirst) {
		m = &modes[0];
		id = idFor(*m);
	} else if (prev == kDisplayModeIDCurrent) {
		m = modeFor(current);
		id = current;
	} else {
		const size_t n = entries();
		size_t i = 0;
		while (i < n && entryId(i) != prev)
			i++;
		if (i == n) {
			// Unknown ID, or kDisplayModeIDFindFirstProgrammable: there
			// are no programmable timings.
			r.csDisplayModeID = kDisplayModeIDInvalid;
			return kBadArgument;
		}
		if (i + 1 == n) {
			r.csDisplayModeID = kDisplayModeIDNoMore;
			return kSuccess;
		}
		m = &entry(i + 1);
		id = entryId(i + 1);
	}
	if (!m) {
		r.csDisplayModeID = kDisplayModeIDInvalid;
		return kBadArgument;
	}
	r.csDisplayModeID    = id;
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
	if (m->id == bootModeId && !isHdrId(r.csTimingMode))
		r.csTimingFlags |= kDisplayModeDefaultFlag;
	return kSuccess;
}

// The range the modes span (IOFBTimingRange): pixel and line rates and every timing field's limits from the mode table (the
// EDID's line-rate limits are not to be trusted: 69-69 kHz on the rig's Samsung), the frame rates from the EDID's range
// limits, variable refresh between them when the EDID has them.
int32_t Translator::getTimingRanges(VDDisplayTimingRangeRec &r) const {
	if (r.csRangeSize && r.csRangeSize < sizeof(VDDisplayTimingRangeRec))
		return kBadArgument;
	Edid::BaseInfo base {};
	if (!count || !Edid::parseBaseBlock(edid, edidLen, base) || !base.range.present || !base.range.minVHz ||
	    base.range.maxVHz <= base.range.minVHz)
		return kBadArgument;
	r = VDDisplayTimingRangeRec {};
	r.csRangeSize = sizeof(VDDisplayTimingRangeRec);
	r.csMinPixelClock = ~0ull;
	r.csMinLineRate = ~0u;
	for (int i = 0; i < 12; i++)
		r.csMinMax[2 * i] = ~0u;
	auto span = [&r](int field, uint32_t v) {
		r.csMinMax[2 * field] = v < r.csMinMax[2 * field] ? v : r.csMinMax[2 * field];
		r.csMinMax[2 * field + 1] = v > r.csMinMax[2 * field + 1] ? v : r.csMinMax[2 * field + 1];
	};
	for (size_t i = 0; i < count; i++) {
		const Edid::DetailedTiming &t = modes[i].t;
		const uint64_t hz = static_cast<uint64_t>(t.pixelClockKHz) * 1000;
		const uint32_t hTotal = t.hActive + t.hBlank, vTotal = t.vActive + t.vBlank;
		const uint32_t line = hTotal ? static_cast<uint32_t>(hz / hTotal) : 0;
		r.csMinPixelClock = hz < r.csMinPixelClock ? hz : r.csMinPixelClock;
		r.csMaxPixelClock = hz > r.csMaxPixelClock ? hz : r.csMaxPixelClock;
		r.csMinLineRate = line < r.csMinLineRate ? line : r.csMinLineRate;
		r.csMaxLineRate = line > r.csMaxLineRate ? line : r.csMaxLineRate;
		r.csMaxHorizontalTotal = hTotal > r.csMaxHorizontalTotal ? hTotal : r.csMaxHorizontalTotal;
		r.csMaxVerticalTotal = vTotal > r.csMaxVerticalTotal ? vTotal : r.csMaxVerticalTotal;
		span(0, t.hActive), span(1, t.hBlank), span(2, t.hSyncOffset), span(3, t.hSyncWidth);
		span(4, t.vActive), span(5, t.vBlank), span(6, t.vSyncOffset), span(7, t.vSyncWidth);
		for (int f = 8; f < 12; f++)
			span(f, 0);                                       // no borders
	}
	// CoreDisplay refused the table's slowest mode (640x480 at the minimum pixel clock) without a margin, so every limit
	// gets one; what is accepted is still decided by the slot match against the table.
	auto lower = [](auto v) { return v - v / 64 - (v ? 1 : 0); };
	auto upper = [](auto v) { return v + v / 64 + 1; };
	r.csMinPixelClock = lower(r.csMinPixelClock), r.csMaxPixelClock = upper(r.csMaxPixelClock);
	r.csMinLineRate = lower(r.csMinLineRate), r.csMaxLineRate = upper(r.csMaxLineRate);
	r.csMaxHorizontalTotal = upper(r.csMaxHorizontalTotal), r.csMaxVerticalTotal = upper(r.csMaxVerticalTotal);
	for (int f = 0; f < 8; f++)
		r.csMinMax[2 * f] = lower(r.csMinMax[2 * f]), r.csMinMax[2 * f + 1] = upper(r.csMinMax[2 * f + 1]);
	for (uint8_t &c : r.csCharSize)
		c = 1;
	r.csTimingRangeSyncFlags = kRangeSupportsSeparateSyncs | kRangeSupportsVRR;
	r.csMinFrameRate = base.range.minVHz;
	r.csMaxFrameRate = base.range.maxVHz;
	// CoreDisplay calls a timing above 165 MHz two links (DVI's rule) and refuses it if the range allows only one.
	r.csMaxNumLinks = 2;
	if (hdr) {
		r.csSupportedPixelEncoding = kRangeEncodingRGB444;
		r.csSupportedBitsPerColorComponent = kRangeBpc8 | kRangeBpc10;
		r.csSupportedColorimetryModes = kRangeColorimetrySRGB | kRangeColorimetryBT2020;
		r.csSupportedDynamicRangeModes = kRangeDynamicSDR | kRangeDynamicHDR10;
	}
	return kSuccess;
}

int32_t Translator::getDetailedTiming(VDDetailedTimingRec &r) const {
	if (r.csTimingSize && r.csTimingSize < sizeof(VDDetailedTimingRec))
		return kBadArgument;
	if (const Slot *sl = adaptiveSync ? slotFor(r.csDisplayModeID) : nullptr) {
		const int32_t id = r.csDisplayModeID;
		if (sl->mode) {
			r = sl->timing;
		} else {
			r = VDDetailedTimingRec {};
			r.csDisplayModeState = kDMSModeFree;
		}
		r.csTimingSize    = sizeof(VDDetailedTimingRec);
		r.csDisplayModeID = id;
		return kSuccess;
	}
	const Modes::Mode *m = modeFor(r.csDisplayModeID);
	if (!m)
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
	if (hdr) {   // both twins say what they are
		const bool pq = isHdrId(id);
		r.csPixelEncoding         = kRangeEncodingRGB444;
		r.csBitsPerColorComponent = pq ? kRangeBpc10 : kRangeBpc8;
		r.csColorimetry           = pq ? kRangeColorimetryBT2020 : kRangeColorimetrySRGB;
		r.csDynamicRange          = pq ? kRangeDynamicHDR10 : kRangeDynamicSDR;
	}
	return kSuccess;
}

int32_t Translator::getConnection(VDDisplayConnectInfoRec &r) const {
	r.csDisplayType       = kGenericLCD;
	r.csConnectTaggedType = 0;
	r.csConnectTaggedData = 0;
	// IONDRVFramebuffer::hasDDCConnect needs both DDC bits.
	r.csConnectFlags      = kReportsDDCConnection;
	if (!connected)
		r.csConnectFlags |= kConnectionInactive;
	else if (edidLen)
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
	const bool same = m == modeFor(current);   // an installed timing for the mode already on needs no switch
	if (!same) {
		int32_t err = be.switchTo ? be.switchTo(be.ctx, *m, m->id == bootModeId) : kUnsupported;
		if (err != kSuccess)
			return err;
	}
	current = r.csData;
	// The backend may place the new mode's surface elsewhere.
	if (!same && !surface(*m, s))
		return kBadArgument;
	// An installed timing with a blanking extension is the mode with variable refresh; anything else runs it fixed.
	const Slot *sl = slotFor(r.csData);
	const uint32_t ext = sl && sl->mode ? sl->timing.csReserved2 : 0;
	if (be.setRefresh && (ext || variable) && be.setRefresh(be.ctx, *m, ext) == kSuccess)
		variable = ext != 0;
	r.csBaseAddr = static_cast<uintptr_t>(1 | s.physBase);
	return kSuccess;
}

// IONDRVFramebuffer::setDetailedTiming: free a slot, or give it a timing and the mode ID (alias) macOS knows it by.
int32_t Translator::setDetailedTiming(const VDDetailedTimingRec &r) {
	const int32_t id = r.csDisplayModeID;
	if (!isSlotId(id) || (r.csTimingSize && r.csTimingSize < sizeof(VDDetailedTimingRec)))
		return kBadArgument;
	Slot &sl = slots[id - kSlotIdBase];
	if (r.csDisplayModeState == kDMSModeFree) {
		sl = Slot {};
		return kSuccess;
	}
	const Modes::Mode *m = tableMatch(r);
	if (!m)
		return kBadArgument;
	for (Slot &other : slots)                                 // one slot per alias
		if (&other != &sl && other.mode && other.timing.csDisplayModeAlias == r.csDisplayModeAlias)
			other = Slot {};
	sl.timing = r;
	sl.timing.csTimingSize = sizeof(VDDetailedTimingRec);
	sl.timing.csDisplayModeSeed = ++slotSeed;
	sl.mode = m;
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
