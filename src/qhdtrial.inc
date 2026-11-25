// Included by plugin.cpp beside its boot-framebuffer hooks. The two apertures
// remain allocated for the lifetime of the framebuffer, including after rollback.
namespace QhdTrial {
enum Rate { home = 0, low = 60, high = 165 };
constexpr uint32_t timeoutSeconds = 40;
FbState *state {};
IODeviceMemory *vram {}, *surface {};
Ndrv::Surface plane {};
uint64_t mc {}, span {}, expires {};
uint32_t lowId {}, highId {};
int active {}, armed {}, fallback {}, retries {};
bool switched {}, confirmedLow {}, confirmedHigh {};
bool nativeStartup {}, startupPending {};
thread_call_t timer {};
enum DeepColor { deepIdle = 0, deepRecovering = -1, deepPreview = -10, deepConfirmed = 10 };
DeepColor deepColor {deepIdle};
thread_call_t deepTimer {};
uint64_t deepExpires {};
unsigned deepRetries {};

int rate() { return __atomic_load_n(&active, __ATOMIC_ACQUIRE); }

void publish(int value) { __atomic_store_n(&active, value, __ATOMIC_RELEASE); }

bool restore() {
	auto &dev = state->dev;
	bool ok = fallback == low ? dev.setDesktopRefresh(0, false) : dev.restoreDesktopTrial();
	if (!ok) return false;
	publish(fallback);
	dev.currentModeId = fallback == low ? lowId : dev.defaultModeId;
	state->ndrv.adoptMode(*dev.findMode(dev.currentModeId));
	armed = 0;
	switched = false;
	return true;
}

void expired(thread_call_param_t, thread_call_param_t) {
	RDNA4Device::ExtraHold hold(state->dev);
	if (!armed || mach_absolute_time() < expires) return;
	if (!switched || restore()) {
		armed = 0;
		return;
	}
	FBLOG("native DP: rollback readback failed (attempt %d)", retries + 1);
	if (++retries < 3) {
		clock_interval_to_deadline(1, kSecondScale, &expires);
		thread_call_enter_delayed(timer, expires);
	}
}

int control(struct sysctl_oid *oid, void *, int, struct sysctl_req *req) {
	RDNA4Device::ExtraHold hold(state->dev);
	// A positive value is a confirmed mode. A negative value is the target of an armed preview.
	int value = armed ? -armed : rate();
	const int err = sysctl_handle_int(oid, &value, 0, req);
	if (err || !req->newptr) return err;
	if (value == 1) {  // confirm only after a successful NDRV switch and before the deadline
		if (!armed || !switched || rate() != armed || mach_absolute_time() >= expires) return EAGAIN;
		if (armed == low) confirmedLow = true;
		if (armed == high) confirmedHigh = true;
		armed = 0;
		switched = false;
		thread_call_cancel(timer);
		return 0;
	}
	if (value == 0 && armed) {
		if (switched && !restore()) return EIO;
		armed = 0;
		thread_call_cancel(timer);
		return 0;
	}
	if (armed || !((value == low && rate() == home && state->dev.currentModeId == state->dev.defaultModeId) ||
	               (value == high && rate() == low))) return EINVAL;
	fallback = rate();
	armed = value;
	switched = false;
	retries = 0;
	clock_interval_to_deadline(timeoutSeconds, kSecondScale, &expires);
	thread_call_enter_delayed(timer, expires);
	return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, rdna4_qhdtrial, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
            nullptr, 0, control, "I", "Desktop DP preview: arm 60 or 165, confirm 1, cancel 0");

int readMeasurement(struct sysctl_oid *oid, void *, int depth, struct sysctl_req *req) {
	int reading = depth ? state->dev.trialDepth() : state->dev.trialRate();
	return sysctl_handle_int(oid, &reading, 0, req);
}
SYSCTL_PROC(_debug, OID_AUTO, rdna4_qhdmhz, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
            nullptr, 0, readMeasurement, "I", "Measured scanout in milliHz");
SYSCTL_PROC(_debug, OID_AUTO, rdna4_qhdbpc, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
            nullptr, 1, readMeasurement, "I", "Verified DisplayPort bits per channel");

int readDpcdSnapshot(struct sysctl_oid *, void *, int, struct sysctl_req *req) {
	RDNA4Device::BootDpcdSnapshot snapshot {};
	static_assert(sizeof(snapshot) == 34, "DPCD snapshot layout");
	if (req->oldptr && !state->dev.readBootDpcdSnapshot(snapshot))
		return EIO;
	return SYSCTL_OUT(req, &snapshot, sizeof(snapshot));
}
SYSCTL_PROC(_debug, OID_AUTO, rdna4_dpcd_caps, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED,
            nullptr, 0, readDpcdSnapshot, "S,BootDpcdSnapshot",
            "Read DPCD base and extended receiver capabilities and current DP link");

bool cancelDeepColor() {
	if (deepColor == deepIdle)
		return true;
	deepColor = deepRecovering;
	if (!state->dev.restoreDesktopDeepColor())
		return false;
	deepColor = deepIdle;
	thread_call_cancel(deepTimer);
	return true;
}

void retryDeepColorRollback() {
	if (++deepRetries <= 3) {
		clock_interval_to_deadline(2, kSecondScale, &deepExpires);
		thread_call_enter_delayed(deepTimer, deepExpires);
	}
	FBLOG("QHD 10 bpc: HBR2 rollback not verified (attempt %u)", deepRetries);
}

void deepColorExpired(thread_call_param_t, thread_call_param_t) {
	RDNA4Device::ExtraHold hold(state->dev);
	if ((deepColor == deepPreview || deepColor == deepRecovering) && mach_absolute_time() >= deepExpires &&
	    !cancelDeepColor())
		retryDeepColorRollback();
}

int controlDeepColor(struct sysctl_oid *oid, void *, int, struct sysctl_req *req) {
	auto &dev = state->dev;
	RDNA4Device::ExtraHold hold(dev);
	int value = deepColor;
	const int err = sysctl_handle_int(oid, &value, 0, req);
	if (err || !req->newptr)
		return err;
	if (value == 0) {
		deepRetries = 0;
		if (cancelDeepColor()) return 0;
		retryDeepColorRollback();
		return EIO;
	}
	if (value == 1) {
		if (deepColor != deepPreview)
			return EAGAIN;
		if (mach_absolute_time() < deepExpires && dev.verifyDesktopDeepColor() &&
		    mach_absolute_time() < deepExpires) {
			deepColor = deepConfirmed;
			thread_call_cancel(deepTimer);
			return 0;
		}
		if (!cancelDeepColor()) retryDeepColorRollback();
		return EIO;
	}
	if (value != 10 || deepColor != deepIdle || armed || !confirmedHigh || rate() != high ||
	    dev.currentModeId != highId)
		return EINVAL;
	deepColor = deepRecovering;
	deepRetries = 0;
	clock_interval_to_deadline(timeoutSeconds, kSecondScale, &deepExpires);
	thread_call_enter_delayed(deepTimer, deepExpires);
	if (!dev.startDesktopDeepColor(mc) || mach_absolute_time() >= deepExpires) {
		if (!cancelDeepColor()) retryDeepColorRollback();
		return EIO;
	}
	deepColor = deepPreview;
	return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, rdna4_qhd10, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
            nullptr, 0, controlDeepColor, "I", "QHD RGB10 trial: start 10, confirm 1, restore HBR2 RGB8 with 0");

void registerControls() {
	sysctl_register_oid(&sysctl__debug_rdna4_qhdtrial);
	sysctl_register_oid(&sysctl__debug_rdna4_qhdmhz);
	sysctl_register_oid(&sysctl__debug_rdna4_qhdbpc);
	sysctl_register_oid(&sysctl__debug_rdna4_dpcd_caps);
	deepTimer = thread_call_allocate(deepColorExpired, nullptr);
	if (deepTimer)
		sysctl_register_oid(&sysctl__debug_rdna4_qhd10);
}

int change(const Modes::Mode &mode) {
	auto &dev = state->dev;
	RDNA4Device::ExtraHold hold(dev);
	const int target = mode.id == highId ? high : mode.id == lowId ? low : home;
	if (deepColor != deepIdle) {
		if (deepColor != deepRecovering && target == high)
			return kIOReturnSuccess; // Same CoreGraphics mode, already running.
		if (!cancelDeepColor()) {
			retryDeepColorRollback();
			return kIOReturnIOError;
		}
	}
	if (!armed && target == high && rate() == high)
		return kIOReturnSuccess;
	if (armed && target != armed) {
		const uint32_t previous = fallback == low ? lowId : dev.defaultModeId;
		if (mode.id != previous) return kIOReturnNotPermitted;
		if (!switched) return kIOReturnSuccess;
		return restore() ? kIOReturnSuccess : kIOReturnIOError;
	}
	if (target == home) {
		if (mode.id != dev.defaultModeId && rate()) return kIOReturnUnsupported;
		if (!rate()) return dev.applyMode(mode);
		if (!dev.restoreDesktopTrial()) return kIOReturnIOError;
		publish(home);
	} else if (rate() == high && target == low && !armed) {
		if (!dev.setDesktopRefresh(0, false)) return kIOReturnIOError;
		publish(low);
	} else if (!armed && ((target == low && confirmedLow) || (target == high && confirmedHigh))) {
		// Settings may select a visually confirmed mode again during this boot.
		const bool fromHome = rate() == home;
		if (fromHome && (dev.currentModeId != dev.defaultModeId || !dev.startDesktopTrial(mc))) {
			if (dev.currentModeId == dev.defaultModeId) (void)dev.restoreDesktopTrial();
			return kIOReturnIOError;
		}
		if (target == high && !dev.setDesktopRefresh(mc, true)) {
			if (fromHome) (void)dev.restoreDesktopTrial();
			else (void)dev.setDesktopRefresh(0, false);
			return kIOReturnIOError;
		}
		publish(target);
	} else {
		if (armed != target || switched || mach_absolute_time() >= expires)
			return kIOReturnNotPermitted;
		switched = true; // a partial plan needs rollback too
		const bool ok = target == high ? dev.setDesktopRefresh(mc, true) : dev.startDesktopTrial(mc);
		if (!ok || mach_absolute_time() >= expires) {
			(void)restore();
			return kIOReturnIOError;
		}
		publish(target);
	}
	dev.currentModeId = mode.id;
	return kIOReturnSuccess;
}

void prepare(FbState &fb, IOPCIDevice *pci) {
#ifdef AMDRadeonRDNA4_QHD_TEST_DEFAULT
	uint32_t enabled = 1;
#else
	uint32_t enabled = 0;
#endif
	(void)Trial::option("rdna4-qhdtrial", enabled);
	auto &dev = fb.dev;
	uint32_t bench = 0, guest = 0, cold = 0;
	if (!enabled || state || !gApertureRouted || !orgGetVRAMRange || !dev.isAmd ||
	    !dev.modesetRequested || pvGpu || (Trial::option("rdna4-pvgpu", guest) && guest) ||
	    (Trial::option("rdna4-coldboot", cold) && cold) ||
	    (Trial::option("rdna4-vrambench", bench) && bench) || dev.modeCount + 2 > Modes::MaxModes)
		return;
	const Modes::Mode *boot = dev.findMode(dev.defaultModeId);
	Edid::DetailedTiming preferred {}, fast {};
	if (!boot || boot->t.hActive != 1920 || boot->t.vActive != 1080 ||
	    !Edid::preferredTiming(dev.edidData, dev.edidLen, preferred) ||
	    preferred.hActive != 2560 || preferred.vActive != 1440 || preferred.pixelClockKHz != 241500)
		return;
	for (size_t off = 256; off + 128 <= dev.fullEdidLen(); off += 128) {
		const uint8_t *block = dev.edidData + off;
		if (!Edid::blockChecksumOk(block)) break;
		Edid::DetailedTiming timings[8] {};
		size_t n = Edid::displayIdTimings(block, timings, 8);
		for (size_t i = 0; i < n; ++i)
			if (timings[i].hActive == 2560 && timings[i].vActive == 1440 &&
			    timings[i].pixelClockKHz == 699500 && timings[i].refreshMilliHz() == 164957)
				fast = timings[i];
	}
	if (!fast.pixelClockKHz) return;
	auto *bar = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	if (!bar || bar->getPhysicalAddress() != dev.fbPhysBase ||
	    !Ndrv::spareSurface(dev.fbPhysBase, dev.fbLength, dev.fbPhysBase,
	                        min(uint64_t(bar->getLength()), RDNA4Compute::kPoolFloor),
	                        2560, 1440, plane)) return;
	const uint64_t offset = plane.physBase - dev.fbPhysBase;
	const uint64_t bytes = (uint64_t(plane.rowBytes) * plane.height + 128 + PAGE_MASK) & ~uint64_t(PAGE_MASK);
	span = offset + bytes;
	uint64_t homeMc = 0;
	if (span > RDNA4Compute::kPoolFloor || span > bar->getLength() ||
	    !dev.trialMemoryOK(span) || !dev.consoleMcAddress(homeMc)) return;
	mc = homeMc + offset;
	vram = IODeviceMemory::withSubRange(bar, 0, span);
	surface = IODeviceMemory::withSubRange(bar, offset, bytes);
	timer = thread_call_allocate(expired, nullptr);
	// Initialize only the newly reserved memory, including the page padding that can be mapped.
	auto *map = surface ? surface->map(kIOMapWriteCombineCache) : nullptr;
	if (!vram || !surface || !timer || !map || !map->getVirtualAddress()) {
		OSSafeReleaseNULL(map); OSSafeReleaseNULL(vram); OSSafeReleaseNULL(surface);
		if (timer) { thread_call_free(timer); timer = nullptr; }
		return;
	}
	bzero(reinterpret_cast<void *>(map->getVirtualAddress()), bytes);
	__asm__ volatile("sfence" ::: "memory");
	map->release();
	const size_t oldCount = dev.modeCount;
	const int l = Modes::ensure(dev.modeTable, dev.modeCount, Modes::MaxModes, preferred, Modes::SourceDtd);
	const int h = Modes::ensure(dev.modeTable, dev.modeCount, Modes::MaxModes, fast, Modes::SourceDisplayId);
	if (l < 0 || h < 0 || l == h) {
		dev.modeCount = oldCount;
		OSSafeReleaseNULL(vram); OSSafeReleaseNULL(surface); thread_call_free(timer); timer = nullptr;
		return;
	}
	lowId = dev.modeTable[l].id;
	highId = dev.modeTable[h].id;
	state = &fb;
	FBLOG("native DP: desktop preview ready: separate %ux%u surface, pitch %u, modes %u/%u",
	      plane.width, plane.height, plane.rowBytes, lowId, highId);
}

bool requestInitialMode(FbState &fb, uint32_t computeStage) {
	uint32_t enabled = 0;
	if (!Trial::option("rdna4-qhdboot", enabled) || enabled != 1)
		return false;
	if (state != &fb || computeStage < RDNA4Compute::StageGfx) {
		FBLOG("native DP startup: QHD surface or SMU bring-up unavailable, keeping firmware mode");
		return false;
	}
	startupPending = true;
	return true;
}

void startInitialMode(RDNA4Device &dev) {
	// Finish all hardware changes before IONDRV asks for current mode,
	// aperture and row pitch. Only publish a verified native surface.
	if (!dev.prepareDesktopStartup()) {
		FBLOG("native DP startup: display clocks unavailable, keeping firmware mode");
		return;
	}
	int selected = home;
	if (dev.startDesktopTrial(mc)) {
		selected = low;
		if (dev.setDesktopRefresh(mc, true)) {
			if (dev.startDesktopDeepColor(mc)) {
				deepColor = deepConfirmed;
				selected = high;
			} else if (dev.restoreDesktopDeepColor() && dev.verifyDesktopRgb8()) {
				selected = high;
			} else {
				selected = home;
			}
		} else if (!dev.setDesktopRefresh(0, false)) {
			selected = home;
		}
	}
	if (selected == home) {
		const bool restored = dev.restoreDesktopTrial() || dev.restoreDesktopTrial();
		FBLOG("native DP startup: firmware mode rollback %s", restored ? "verified" : "failed");
		return;
	}

	nativeStartup = true;
	confirmedLow = true;
	confirmedHigh = selected == high;
	dev.currentModeId = selected == high ? highId : lowId;
	// This scanout can switch only among the two native modes and the
	// firmware surface. Drop the other GOP-pitch entries at publication so
	// WindowServer cannot restore an unusable mode saved on an earlier boot.
	size_t count = 0;
	for (size_t i = 0; i < dev.modeCount; ++i) {
		const auto &mode = dev.modeTable[i];
		if (mode.id == dev.defaultModeId || mode.id == lowId || mode.id == highId)
			dev.modeTable[count++] = mode;
	}
	dev.modeCount = count;
	publish(selected);
	FBLOG("native DP startup: initial mode %u, 2560x1440, %d mHz, RGB %d bpc%s",
	      dev.currentModeId, dev.trialRate(), dev.trialDepth(),
	      deepColor == deepConfirmed ? ", HBR3 x4" : selected == high ? ", HBR2 x4" : "");
}

uint64_t finishInitialMode(RDNA4Device &dev) {
	if (!state || &state->dev != &dev)
		return 0;
	RDNA4Device::ExtraHold hold(dev);
	if (!startupPending)
		return 0;
	startInitialMode(dev);
	startupPending = false;
	IORecursiveLockWakeup(dev.extraLock, &startupPending, true);
	const int measured = nativeStartup ? dev.trialRate() : 0;
	return measured > 0 ? 1000000000000ull / measured : 0;
}

void waitForInitialMode(RDNA4Device &dev) {
	RDNA4Device::ExtraHold hold(dev);
	uint64_t deadline;
	clock_interval_to_deadline(12, kSecondScale, &deadline);
	while (startupPending && !RDNA4Compute::bringupEnded() && mach_absolute_time() < deadline)
		IORecursiveLockSleepDeadline(dev.extraLock, &startupPending, deadline, THREAD_UNINT);
	if (startupPending) {
		startupPending = false;
		FBLOG("native DP startup: SMU did not become ready in time, keeping firmware mode");
	}
}
} // namespace QhdTrial
