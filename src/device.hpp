//
//  device.hpp
//  AMDRadeonRDNA4
//
//  Hardware side of AMDRadeonRDNA4 for one GPU: the GOP-programmed scanout, BAR5
//  register access, VBIOS / IP discovery, the lit display pipe, the DP AUX
//  and DC_I2C engines (EDID), the DMUB mailbox, display power and the
//  display-mode table.
//
//  It is a plain C++ object owned by the Lilu plugin (see plugin.cpp), which
//  creates one when Apple's IONDRVFramebuffer opens the GPU's boot display
//  and answers that driver's NDRV requests from it. Registry properties are
//  published on the owning framebuffer service.
//

#ifndef RDNA4Device_hpp
#define RDNA4Device_hpp

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLocks.h>
#include <pexpert/pexpert.h>

#include "atombios.hpp"
#include "dmub.hpp"
#include "ipdiscovery.hpp"
#include "modes.hpp"
#include "modeset.hpp"
#include "ndrv.hpp"
#include "pipe.hpp"
#include "pipe2.hpp"
#include "dptrain.hpp"
#include "regsnap.hpp"

class IOMemoryMap;
struct SmuMailbox;

class RDNA4Device {
public:
	// `pci` is the GPU, `owner` the service registry properties go on
	// (the IONDRVFramebuffer). Returns false if the boot framebuffer is
	// unusable, in which case the plugin leaves the display alone.
	bool init(IOPCIDevice *pci, IOService *owner);
	~RDNA4Device();

	// True for the AMD GPU; false for the VM test device (vmware-svga),
	// where all register work is skipped.
	bool isAmd { false };

	// Console framebuffer geometry taken from PE_state.video.
	IOPhysicalAddress64 fbPhysBase { 0 };
	uint64_t            fbLength   { 0 };
	uint32_t            fbWidth    { 0 };
	uint32_t            fbHeight   { 0 };
	// The size of the boot pipe's running mode (applyMode keeps it), false before it is known.
	bool liveSize(uint32_t &width, uint32_t &height) const {
		width = liveTiming.hActive;
		height = liveTiming.vActive;
		return liveTimingValid;
	}
	// The boot pipe's running mode, or nullptr before it is known.
	const Edid::DetailedTiming *liveMode() const { return liveTimingValid ? &liveTiming : nullptr; }
	uint32_t            fbRowBytes { 0 };
	uint32_t            fbDepth    { 32 };

	// EDID of the boot display's sink: base block plus one CTA extension if
	// present. 0 length = no DDC sink found.
	uint8_t edidData[256] {};
	size_t  edidLen { 0 };
	// Blocks 2 and 3 from E-DDC segment 1, assembled in edidAll for the boot framebuffer.
	uint8_t edidMore[256] {};
	size_t  edidMoreLen { 0 };
	uint8_t edidAll[512] {};   // edidData and edidMore together: every block the sink has (up to four)
	size_t  edidAllLen { 0 };
	// A display beyond the boot one (rdna4-head2, plugin.cpp): the sink on
	// another connector, the pipe it is lit on and what the kext lit for it.
	// The second-pipe functions below act on the selected one (selectExtra).
	struct ExtraPipe {
		uint8_t       pipe { 1 };          // OTG = HUBP = DPP = MPCC = OPP instance, 1 to 3
		uint8_t       edid[256] {};        // base block plus one extension
		size_t        edidLen { 0 };       // 0: no sink
		uint8_t       hpd { 0 };           // the HPD pin of the connector it is on
		Pipe2::Target target {};           // .now: the timing the pipe runs
		uint16_t      connectorObjId { 0 };
		bool          lit { false }, on { false };
		bool          moveOwed { false };  // secondPipeTo: the move to the connector in use has not completed
		bool          uhd { false };       // its table lights it at 3840x2160 (Pipe2::isUhd)
		bool          hdmi { false };      // its table is for an HDMI stream (on a DisplayPort connector: DP++)
		bool          y420 { false };      // ... in YCbCr 4:2:0 (Pipe2::Config::y420)
		bool          moveToPlan { false };   // the move owed goes to the table's own timing (adoptSinkKind)
		// On DisplayPort: the sink's AUX channel and capabilities (read with
		// its EDID), and the link picked for the mode it is to carry.
		uint8_t       aux { 0 };
		DpTrain::Sink sink {};
		DpTrain::Link link {};
		RegSnap::Snap *snap { nullptr };   // rdna4-wakediff=1: what lighting left (regsnap.hpp)
	};
	static constexpr size_t kMaxExtra = 3;
	ExtraPipe extra[kMaxExtra] { { 1 }, { 2 }, { 3 } };
	size_t extraSlot { 0 };
	// coldStart: while it lights pipe 0 itself from a second-pipe table, the plan's training and AUX writes take
	// the boot display's sink and link from this entry instead of a slot's (held under extraLock).
	ExtraPipe *xpOverride { nullptr };
	ExtraPipe &xp() { return xpOverride ? *xpOverride : extra[extraSlot]; }
	const ExtraPipe &xp() const { return xpOverride ? *xpOverride : extra[extraSlot]; }
	// The one the second-pipe functions act on, and its connector's table
	// (Pipe2::use) once it has a sink.
	void selectExtra(size_t slot);
	void useSlotTable();   // the selected slot's connector's table (Pipe2::use)
	// Held from a selectExtra through everything done with the pipe it selects (ExtraHold): the hot-plug poll
	// (a timer on each head's work loop), macOS's mode switches (the paravirtual host's thread, or NDRV) and
	// the heads' start-up each select a pipe and use it, and did so under one another (card, 2026-11-14: a
	// mode switch logged another slot's empty mode). Recursive: the poll's plug and unplug take it again.
	IORecursiveLock *extraLock { nullptr };
	struct ExtraHold {
		IORecursiveLock *lock;
		explicit ExtraHold(RDNA4Device &dev) : lock(dev.extraLock) { if (lock) IORecursiveLockLock(lock); }
		~ExtraHold() { if (lock) IORecursiveLockUnlock(lock); }
		ExtraHold(const ExtraHold &) = delete;
		ExtraHold &operator=(const ExtraHold &) = delete;
	};
	// How many displays beside the boot one this boot may light (1 to 3,
	// rdna4-heads); the first extra pipe without a sink (kMaxExtra if none);
	// whether another extra pipe than the selected one has that connector.
	static size_t extraHeads();
	size_t freeExtraSlot() const;
	bool hpdTakenByOther(uint8_t hpd) const;

	// Bit (pin - 1): a DisplayPort connector whose sink answered on DDC,
	// i.e. an HDMI monitor behind a passive DP++ adapter.
	uint8_t dpDualHpds { 0 };

	bool dpDual(uint8_t hpd) const {
		return hpd >= 1 && hpd <= 8 && (dpDualHpds & (1u << (hpd - 1)));
	}

	void setDpDual(uint8_t hpd, bool on) {
		if (hpd < 1 || hpd > 8)
			return;

		const uint8_t bit = static_cast<uint8_t>(1u << (hpd - 1));
		if (on) {
			dpDualHpds |= bit;
		} else {
			dpDualHpds &= static_cast<uint8_t>(~bit);
			dpDualKHz[hpd - 1] = 0;
		}
	}

	// The TMDS clock the DP++ adapter on that pin carries (dpdual.hpp); 0
	// while its ID block has not been read.
	uint32_t dpDualKHz[8] {};

	void overrideDpDual(uint8_t hpd);
	uint32_t dpDualMaxKHz(uint8_t hpd) const {
		return dpDual(hpd) ? dpDualKHz[hpd - 1] : 0;
	}

	// Display mode table (modes.hpp). By default it holds only the boot mode;
	// with boot-arg "rdna4-modeset=1" it also carries the sink's EDID modes.
	// Larger DP modes have a separate, permanently reserved aperture. modeCount 0 =
	// the live timing is unknown and the display is left to the boot NDRV.
	Modes::Mode modeTable[Modes::MaxModes] {};
	size_t      modeCount { 0 };
	uint32_t    currentModeId { 0 };   // Modes::Mode::id of the live mode
	uint32_t    defaultModeId { 0 };
	bool        modesetRequested { false };
	// Physical image size from the EDID. 0 = unknown.
	uint32_t    imageWidthMm { 0 }, imageHeightMm { 0 };
	bool dpPlaneReady();
	void prepareDpSurface(uint64_t extraSlotsBytes, bool clocksExpected);
	uint64_t displayReserve {};
	IODeviceMemory *dpVram {}, *dpAperture {};
	Ndrv::Surface dpSurface {};
	bool largerThanConsole(const Edid::DetailedTiming &t) const { return t.hActive > fbWidth || t.vActive > fbHeight; }
	const Modes::Mode *activeDpMode() const {
		const uint32_t id = __atomic_load_n(&dpActiveId, __ATOMIC_ACQUIRE);
		return id ? findMode(id) : nullptr;
	}
	uint64_t displaySpan() const { return dpVram ? dpVram->getLength() : fbLength; }
	const Modes::Mode *findMode(uint32_t id) const;
	// Program mode `m` on the lit HDMI pipe (modeset.hpp). On failure the
	// previous timing is programmed back and an error returned.
	IOReturn applyMode(const Modes::Mode &m);
	// rdna4-boot4k: the boot display behind a passive DP++ adapter, which the
	// firmware lights at what the adapter carries (2560x1440), taken over by a
	// head's pipe that lights its connector at 3840x2160 in YCbCr 4:2:0, as
	// the heads do. bootUhdPipe: whether the head on extra pipe `pipeIndex`
	// can (a table for the boot connector on that pipe, and a sink that takes
	// it). releaseBootPipe: the firmware's stream taken down (stream and link
	// encoder, transmitter; its plane and timing generator run on); the boot
	// pipe has no connector from then on.
	bool bootUhdPipe(uint8_t pipeIndex) const;
	bool releaseBootPipe();
	bool bootReleased { false };

	// The second pipe (pipe2.hpp), for a head whose surface is at CPU
	// address `surfacePhys` in the console's memory range. `level` is
	// rdna4-head2: 2 writes nothing and publishes what every step would do,
	// 3 lights the stream with a solid colour (and publishes what the plane
	// would then do), 4 also the plane.
	bool lightSecondPipe(uint32_t level, IOPhysicalAddress64 surfacePhys);
	// Read-only: the colour-path registers of the pipe the plan lit next to
	// the firmware's pipe, where they differ ("pipe2: colour:" lines).
	void comparePipeColour(const Pipe2::Plan &plan);
	// Hot-plug (plugin.cpp polls). secondSinkHpd: the HPD pin of the lit second
	// pipe's connector if it is high; before the pipe is lit, of the first
	// connector a plan exists for that is high and not the boot display's; else
	// 0. readSecondEdid: the EDID of what is on that connector, into xp().edid.
	// `orAnother`, for a lit pipe whose display is gone: if its own connector is
	// empty, another connector the pipe can be moved to (secondPipeTo).
	uint8_t secondSinkHpd(bool orAnother = false);
	// Take the second-pipe plan for a connector. A DisplayPort one only with
	// rdna4-head2dp=1: lighting a second display over DisplayPort has not run
	// on the card yet (docs/todo-dptest.md), the HDMI plans have.
	// An HDMI connector's 3840x2160 plan is taken once the display clocks are
	// raised for it (uhdClocks), if the sink has SCDC and the plan's mode.
	bool usePlanFor(uint8_t hpd);
	bool dpSinkTakes(const Edid::DetailedTiming &t) const;
	bool sinkTakesUhd(uint8_t hpd, bool *y420 = nullptr) const;
	bool adoptSinkKind(uint8_t hpd);
	bool secondPipeY420(const Edid::DetailedTiming &t) const;
	// DET buffer shares when a pipe is lit (lightSecondPipe).
	static constexpr uint32_t kMinDetSegments = 6;
	bool pipeInUse(uint32_t p) const;
	void freeIdleDet(const Pipe2::Config &c);
	uint32_t detShare(uint32_t plan, uint32_t others) const;
	uint32_t shrinkCompbuf(const Pipe2::Config &c, uint32_t others);
	void logUnderflow(const char *when);
	// rdna4-wakediff=1: snapshot after lighting, compared after each wake.
	void snapAfterLight(const Pipe2::Plan &plan);
	void diffAfterWake();
	static uint32_t snapRead(void *ctx, uint8_t seg, uint32_t dword);
	static void snapDiff(void *ctx, const RegSnap::Reg &was, uint32_t now);
	volatile bool uhdClocks { false };   // set by smuStarted, on the compute thread
	bool uclkHeld { false }, uclkKnown { false };   // memoryClockFollowDisplays: what UCLK's hard minimum is now
	bool pipe2DpAllowed { false };
	bool readSecondEdid(uint8_t hpd);
	// Display sleep and wake of the pipe lightSecondPipe lit, as amdgpu does
	// DPMS off and on for an HDMI stream (Pipe2::Part): the sink loses the
	// signal and sleeps; the timing generator and the plane keep running.
	void setSecondPipePower(bool on);
	// The lit pipe's display is back, on connector `hpd`: wake the link there.
	// On another HDMI connector than before the pipe is moved to it: the
	// mode-set engine clocks the pipe from that connector's PLL and brings its
	// encoder up, then that connector's own sleep and wake.
	bool secondPipeTo(uint8_t hpd);
	// A mode switch on that pipe: the mode-set engine (modeset.hpp) pointed at
	// its blocks, with what else changes with the mode (Pipe2::modeSteps),
	// then the AVI infoframe for the new timing. The surface and its pitch
	// stay; modes are held to secondPipeModeLimits() and secondPipeModeOk().
	IOReturn applySecondPipeMode(const Modes::Mode &m);
	// Variable refresh on the second pipe's DisplayPort stream in mode `m`: down to `extLines` lines more than its V
	// total, or fixed (0). src/drr.hpp.
	IOReturn setSecondPipeRefresh(const Modes::Mode &m, uint32_t extLines);
	void secondPipeKeepMsaIgnore();
	// The same for the boot display (the firmware keeps its link: the bit is looked at after its pin comes back).
	IOReturn setBootRefresh(const Modes::Mode &m, uint32_t extLines);
	void bootKeepMsaIgnore();
	uint32_t otgFramesInASecond(uint8_t otg);   // debug.rdna4_otghz
	IOReturn setPipeRefresh(uint8_t otg, uint8_t aux, const Edid::DetailedTiming &t, uint32_t extLines);
	void keepMsaIgnore(uint8_t otg, uint8_t aux);
	Modes::Limits secondPipeModeLimits() const;
	// Whether the pipe can be switched to `t` (Pipe2::modeSteps): Linux's
	// values for it are in the table or it is near the lit mode's pixel
	// clock, and the DET buffer has the segments it needs.
	bool secondPipeModeOk(const Edid::DetailedTiming &t, const char **why = nullptr);
	uint32_t secondPipeDet(const Edid::DetailedTiming &t) const;
	void clampDet(ModeSet::Step *steps, size_t n, uint32_t segments) const;
	// A DisplayPort second display whose HPD pin dropped and came back without
	// counting as unplugged: its link is trained again if the sink says it is
	// down. Nothing else maintains a link the kext trained.
	void secondLinkBack();
	bool runSecondPipePart(Pipe2::Part part, size_t *steps = nullptr);

	// The boot display's DisplayPort link (rdna4-dptrain, a diagnostic). The
	// firmware trained it and keeps it up itself. noteBootDpLink reads from the
	// sink what it can do and what it was told; logBootLink logs the link as
	// the sink and the encoder have it; retrainBootLink is the self-test of
	// the kext's own training on that link: stream off, transmitter off,
	// DpTrain::bringUp, stream encoder tied to the link, video restarted.
	void noteBootDpLink();
	bool readBootDpLink(DpTrain::Sink &sink, DpTrain::Link &link);
	// What a DisplayPort boot display's mode may clock at: what its trained link carries at 8 bpc, and the DISPCLK
	// the kext holds (dispclkTargetKHz); the boot pixel clock if either is not known. The link is not retrained.
	uint32_t bootDpMaxKHz();
	uint32_t bootDpLinkKHz { 0 };   // what the firmware's link carries at 8 bpc, read when the mode table is built
	uint32_t dispclkTargetKHz();
	DpTrain::Result retrainBootLink(const char *why);
	// rdna4-dcninit=1, a test of lighting the displays when nothing set them up, on a warm card, in a cold start's
	// order: once the kext's DMUB firmware runs and the display clocks are raised (smuStarted). The lit extra pipes
	// are put to sleep, the firmware's boot pipe (DisplayPort, pipe 0, DIG0, link 0, at its 3840x2160 timing) taken
	// down, the display hardware started as Linux's dcn401_init_hw does from nothing (src/dcninit.cpp), and pipe 0
	// lit again from a Linux-generated table with the kext's own link training. `relight`: the extra slots that were
	// lit, now off, for the heads to light again. False when not asked or not done.
	bool coldStart(uint32_t &relight);
	// An NVRAM breadcrumb (the compute runtime's rdna4-trail) for a step that can hang the machine, with the timing
	// generators running now (OTG0-3 master enables) after it: the next boot logs where the last one stopped.
	void breadcrumb(const char *step);
	// rdna4-syssleep: system sleep with the card (on unless =0; not in VM test builds, which have no card to sleep).
	static bool systemSleepWanted();
	// debug.rdna4_head2sleep: 1 puts the second display's pipe to sleep (setSecondPipePower(false)), 2 wakes it, with an
	// NVRAM breadcrumb before each register step and a stop when the card no longer answers (OTG0 reads 0xffffffff):
	// the test of a DisplayPort extra pipe's sleep sequence, which hung the card in the cold start (2026-11-15).
	void head2SleepTest(int what);
	const char *stepTrail { nullptr };   // set while head2SleepTest runs: runSteps' per-step breadcrumbs
	// After a power loss, the selected DisplayPort extra pipe's DDC pads in AUX mode again before it is lit (as the cold
	// start sets the boot display's): a reset leaves them in I2C mode, and no AUX transaction gets a reply.
	void extraAuxPadsAfterPowerLoss();
	// rdna4-coldprobe=1 and `sudo sysctl debug.rdna4_coldreset=1`: the card reset (mode 1) and brought back as far
	// as Linux does before its IP blocks start (stage (b) of a real cold start). The displays go dark; reboot after.
	bool coldReset();
	// rdna4-coldboot=1: a real cold start at boot. What the GOP left is read first (the pipes, EDIDs, the boot link,
	// the scanout address and the DCHUB timer below), then the card reset; the displays light once the SMU runs
	// (coldStartHeads), as after a power loss. True from the reset on.
	class RDNA4Compute *compute { nullptr };   // the runtime beside this device (its sleep and wake: see sleepWakeMessage)
	bool coldBoot { false };   // also from a wake (rdna4-syssleep): the card lost its power and its state
	bool coldStartPending { false };   // a wake's power loss whose cold start (coldStartHeads) has not run yet
	void captureGopState();    // the GOP's scanout address, DCHUB timer and boot link, for a later cold start
	uint32_t vramMark { 0 };   // what sleepWakeMessage wrote into VRAM before sleep (0: nothing)
	uint64_t coldGopSurface { 0 };
	// Where HUBP0 scans (HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS and _HIGH): the GOP's console on a warm card.
	uint64_t bootScanout() const { return regReadDmu(2, 0x060a) | static_cast<uint64_t>(regReadDmu(2, 0x060b) & 0xffff) << 32; }
	uint32_t coldGopTimer { 0 };
	void takeExtraDown();
	bool consoleMcAddress(uint64_t &mc);
	void logBootLink(const char *when);
	const char *auxWhy { "" };   // why the last AUX transaction gave no reply, and the status it ended with
	uint32_t    auxSts { 0 };
	bool bootSinkPresent();              // the boot display's HPD pin
	DpTrain::Sink bootDpSink {};
	DpTrain::Link bootDpLink {};
	bool bootDpLinkKnown { false };
	uint32_t dpTrainLevel { 0 };         // rdna4-dptrain: 1 watch and log, 2 also retrain once by itself

	// Display power (DPMS). Off = disable the DP video stream and put the
	// sink in D3 via DPCD SET_POWER, or on an HDMI pipe blank to black via
	// the display pattern generator; on reverses it. "rdna4-nosleep=1"
	// makes it a no-op.
	void setDisplayPower(bool on);
	bool displayPowerOn { true };
	// The last 16 display power events (macOS display sleep / wake as the kext handled them), kept in the registry property AMDRadeonRDNA4,DisplayPower
	// (" ## " separated, newest last): the kernel log wraps, the registry does not. One small string is rebuilt per event (events are rare).
	void displayPowerNote(bool on, const char *path, const char *result);
	char     displayPowerHist[16][72] {};
	uint32_t displayPowerEvents { 0 };

	// NDRV hardware-cursor csc backend. Cursor memory and register programming
	// are enabled only by rdna4-cursor=1; the default remains software cursor.
	bool supportsHardwareCursor() const { return hwCursorReady; }
	bool ndrvHardwareCursor() const { return hwCursorReady && ndrvCursorRequested; }   // IONDRV's cscSupportsHardwareCursor: rdna4-cursor=1 only
	IOReturn setHardwareCursor(void *cursorRef);
	IOReturn drawHardwareCursor(int32_t x, int32_t y, uint32_t visible);
	// The paravirtual display's cursor (PvScanout::cursorImage): straight ARGB pixels, `pitch` pixels a row, at most 64x64.
	IOReturn setCursorPixels(const uint32_t *argb, uint32_t w, uint32_t h, uint32_t pitch, uint32_t hotX, uint32_t hotY);
	void setCursorPipe(uint8_t p) { cursorPipe = p; }   // 0xff: the device's own again
	uint32_t cursorHotX() const { return hwCursorHot[cursorIndex()][0]; }   // the hot spot of the pipe the cursor calls program
	uint32_t cursorHotY() const { return hwCursorHot[cursorIndex()][1]; }
	// Once the power-management firmware runs its features (compute stage
	// 3's thread calls it): the display clocks, logged with rdna4-clocks=1,
	// raised for a 3840x2160 pipe (rdna4-dispclk=<MHz> overrides, 0 = off).
	void smuStarted();
	void cursorRegProbe(const char *why);   // W32: read-only cursor plane dump (also called from the compute thread after a flip)
	IOReturn getHardwareCursorDrawState(Ndrv::VDHardwareCursorDrawStateRec &state) const;

	// Shared with the compute bring-up (compute.hpp): the BAR5 mapping and
	// the IP discovery table. Null until init() found them.
	volatile uint32_t *mmioBase() const { return rmmio; }
	size_t mmioSize() const { return rmmioSize; }
	const IpDiscovery *discovery() const { return ipDiscovery.isValid() ? &ipDiscovery : nullptr; }
	uint64_t liveFramePeriodNs() const;

private:
	IOPCIDevice *pciDevice { nullptr };
	IOService   *owner { nullptr };

	bool captureConsoleInfo();

	// VBIOS image (owned copy) and its parsed view.
	uint8_t  *vbiosData { nullptr };
	size_t    vbiosSize { 0 };
	AtomBios  atomBios;
	bool loadVBIOS();
public:
	const uint8_t *vbiosBytes(size_t &size) const { size = vbiosSize; return vbiosData; }
private:
	bool copyVBIOSFromProperty();
	bool copyVBIOSFromExpansionROM();
	void publishVBIOSInfo();
	void freeVBIOS();

	// Register bases from the IP discovery binary (full flash dump injected
	// as ATY,bin_image, or the PSP's on-die copy).
	IpDiscovery ipDiscovery;
	const SmuMailbox *smu { nullptr };   // the firmware mailbox that answers (clk.cpp), once it runs
	uint8_t *onDieDisc { nullptr };
	bool loadOnDieDiscovery();

	// Register MMIO aperture (PCI BAR5).
	IOMemoryMap       *rmmioMap { nullptr };
	volatile uint32_t *rmmio    { nullptr };
	size_t             rmmioSize { 0 };
	bool mapRegisters();
	void unmapRegisters();
	// Bounds-checked 32-bit MMIO read; returns 0xFFFFFFFF when unmapped or
	// out of range.
	uint32_t regRead32(uint32_t byteOffset) const;
	// DCN/DMU register by IP base-segment index + dword offset.
	uint32_t regReadDmu(uint8_t baseIdx, uint32_t dwordOffset) const;
	bool regWriteDmu(uint8_t baseIdx, uint32_t dwordOffset, uint32_t value);

	void tryForce8bpc();
	void probeMemSize();
	void coldProbe();   // rdna4-coldprobe=1: what a mode-1 reset and asic_init depend on, read only
	// The card's PCI config space (and its PCIe link, power state, Resizable BAR), the two bridges of its own PCIe
	// switch above it, and three registers over MMIO: what a reset or a sleep leaves (`when` names the moment).
	void logPciState(const char *when);
	// rdna4-syssleep=1: logPciState at the system's sleep and wake (roadmap item 5, stage 2).
	IONotifier *sleepWakeNotifier { nullptr };
	// Without rdna4-pm the runtime's own sleep and wake are driven from here: powerWillSleep at the system's sleep
	// notice, powerDidWake once the card's PCI device has powered on again (its config space restored).
	IONotifier *pciPowerNotifier { nullptr };
	bool drivesRuntimePower() const;
	static IOReturn pciPowerMessage(void *target, void *ref, UInt32 type, IOService *provider, void *arg, vm_size_t size);
	static IOReturn sleepWakeMessage(void *target, void *ref, UInt32 type, IOService *provider, void *arg, vm_size_t size);
	void dumpDCN();

	// DP AUX software engine and DC_I2C hardware engine (EDID).
	uint32_t auxDword(uint8_t inst, uint32_t reg) const;
	int auxTransaction(uint8_t inst, uint8_t action, uint32_t address,
	                   const uint8_t *data, uint8_t len,
	                   uint8_t *reply, uint8_t replyCap, uint8_t *replyBytes);
	bool readEDID(uint8_t inst, uint8_t *edid, size_t count, uint8_t start, uint8_t segment = 0);
	void readMoreEdidBlocks(uint8_t inst, uint8_t blocks);
	bool readEDIDI2C(uint8_t line, uint8_t *edid, size_t count, uint8_t start);
	// A DisplayPort connector's DDC pads in I2C (true) or AUX (false) mode.
	// A passive DP++ (DP-to-HDMI) adapter carries the sink's DDC on them,
	// which the DC_I2C engine reaches in I2C mode only (hw_ddc.c set_config).
	void ddcPadMode(uint8_t line, bool i2c);
	bool readEDIDDpDual(uint8_t line, uint8_t *edid, size_t count);
	// The DP++ adapter's ID block on that pin's DDC line, pads in I2C mode.
	void readDpDualAdaptor(uint8_t hpd, uint8_t line);
	// The sink's DPCD over native AUX, any length (16 bytes a transfer).
	bool dpcdRead(uint8_t aux, uint32_t address, uint8_t *data, size_t len);
	bool dpcdWrite(uint8_t aux, uint32_t address, const uint8_t *data, size_t len);
	bool i2cTransfer(uint8_t line, uint8_t addr, const uint8_t *wr, size_t wlen, uint8_t *rd, size_t rlen);
	bool scdcConfigure(uint8_t line, uint8_t tmdsConfig);
	bool scdcConfigureWait(uint8_t line, uint8_t tmdsConfig);
	void scdcRefresh();
	void probeEDID();
	// AUX engine of the sink whose EDID we cached — the target for DPCD
	// power writes.
	uint8_t sinkAuxInst  { 0 };
	bool    sinkAuxValid { false };

	// Diagnostics (boot-arg gated, read-mostly).
	void dmubPing();
	void dmubHistory();
	void dumpDmubVersion();
	void smuPing();
	// The power-management firmware's mailboxes (clk.cpp).
	bool smuRegs(const SmuMailbox &mb, uint32_t &msg, uint32_t &arg,
		     uint32_t &resp) const;
	uint32_t smuWait(uint32_t resp) const;
	bool smuSend(const SmuMailbox &mb, uint32_t id, uint32_t param,
		     uint32_t *out);
	bool smuProbe(const SmuMailbox &mb);
	const SmuMailbox *smuFind();
	bool smuHardMinMet(uint32_t clk);
	bool smuSetHardMin(uint32_t clk, const char *name, uint32_t mhz);
	bool dscLit { false };   // rdna4-dscboot: pipe 0 runs Linux's DSC test stream (coldStart); mode switches refused
	bool readReceiverCaps(uint8_t aux, uint8_t (&caps)[16], DpTrain::Sink &sink);   // DPCD 0x000, or the extended block at 0x2200 (Linux's rule)
	void readDscCaps(uint8_t aux, DpTrain::Sink &sink, const char *who);   // DPCD 1.4: the sink's DSC decoder, logged
	void memoryClockFollowDisplays();   // UCLK held at its top level while an extra display runs a 4K-class mode (clk.cpp)
	bool raiseDisplayClocks();
	uint32_t topDpmLevel(uint32_t clk);
	void logDisplayClocks();
	uint32_t dpmLevelCount(const SmuMailbox &mb, uint32_t clk);
	void logDpmLevels(const SmuMailbox &mb, uint32_t clk, const char *name);
	void logDispclkDivider();
	void dumpIH();
	void dumpPSP();
	void dumpModeState();

	// Indirect VRAM dword access via MM_INDEX/MM_DATA.
	uint32_t vramRead32(uint64_t pos);
	void     vramWrite32(uint64_t pos, uint32_t value);

	// The DMUB inbox1 ring the GOP initialised.
	struct DmubRing {
		uint64_t base { 0 };
		uint32_t size { 0 };
	};
	bool dmubRing(DmubRing &ring);
	bool dmubSubmit(const Dmub::Cmd *cmds, uint32_t count, const char *tag,
	                uint64_t *firstSlot = nullptr);
	// rdna4-dmubload: the entries the kext's own DMUB firmware added to its trace buffer since the last call.
	void dmubTrace(const char *tag);
	uint32_t dmubTraceSeen { 0 };

	// The display pipe the GOP lit (see pipe.hpp).
	Pipe::State pipe;
	Edid::DetailedTiming bootTiming {};
	bool bootTimingValid { false };
	void discoverPipe();
	uint64_t measureFramePeriodNs() { return measureFramePeriodNs(otgOff()); }
	uint64_t measureFramePeriodNs(uint32_t otgOffset);
	static uint32_t pipeRead(void *ctx, uint8_t baseIdx, uint32_t dword);
	uint32_t otgOff()  const { return pipe.otg  < Pipe::kMaxOtg ? pipe.otg  * Pipe::Reg::kOtgStride  : 0; }
	uint32_t hubpOff() const { return pipe.hubp < Pipe::kMaxOtg ? pipe.hubp * Pipe::Reg::kHubpStride : 0; }
	uint32_t dppOff()  const { return pipe.hubp < Pipe::kMaxOtg ? pipe.hubp * 0x16bu : 0; }
	uint32_t oppOff()  const { return pipe.opp  < Pipe::kMaxOtg ? pipe.opp  * Pipe::Reg::kOppStride  : 0; }
	uint32_t digOff()  const { return pipe.dig  < Pipe::kMaxDig ? pipe.dig  * Pipe::Reg::kDigStride  : 0; }

	bool displaySleepEnabled { true };
	uint32_t dpgSavedControl { 0 };
	// Release the GOP's OTG master update lock and make sure a VUPDATE latch
	// pulse exists, so double-buffered pipe writes become live. Idempotent.
	bool updateLatchReady { false };
	void ensureUpdateLatch();

	void buildModeTable();

	// The timing on the pipe now: the boot timing, then each mode applied.
	Edid::DetailedTiming liveTiming {};
	bool liveTimingValid { false };
	// The mode whose request timing (HUBP DLG/TTU) the boot plane has: the GOP's, until a switch with Linux's values.
	Edid::DetailedTiming reqTiming {};
	bool reqTimingValid { false };
	ModeSet::Plan modePlan {}, surfaceReturn {};
	uint64_t dpPlaneMc {}, dpConsoleMc {};
	uint32_t dpActiveId {};
	bool surfaceRecovery {};
	uint32_t dpSurfaceClockLimit();
	bool singleDpPipe() const;
	bool bootEdidMatches();
	bool surfaceLatched(uint64_t mc, uint32_t pitch, uint32_t width, uint32_t height);
	bool restoreDpPlane();
	uint64_t returnMc {};
	uint32_t returnPitch {}, returnWidth {}, returnHeight {};
	bool pathForHpd(uint8_t hpdPin, AtomBios::DisplayPath &out, uint8_t *ddcLine = nullptr);
	// The lit pipe and its connector's wiring as a mode-set target (timings not filled in).
	bool bootPipeTarget(ModeSet::Target &t);
	bool hdmiLinkOff { false };   // display sleep took the HDMI boot display's link down (setDisplayPower)
	bool runPlan(const ModeSet::Plan &plan) {
		return runSteps(plan.steps, plan.count, plan.cmds, plan.ncmds, "modeset", otgOff());
	}
	// `otgOffset`: the timing generator whose frames an Op::WaitFrames counts.
	// `aux`: what the Op::Aux steps of a DisplayPort plan write.
	bool runSteps(const ModeSet::Step *steps, size_t count, const Dmub::Cmd *cmds, size_t ncmds,
	              const char *tag, uint32_t otgOffset, const Pipe2::AuxWrite *aux = nullptr);
	DpTrain::Result trainLink(uint8_t aux, uint8_t link, uint8_t hpd, uint8_t conn, const DpTrain::Sink &sink,
	                          const DpTrain::Link &settings, bool offFirst, DpTrain::Report &rep);
	// The second pipe on DisplayPort: its sink's capabilities (with its EDID),
	// and the link for the mode it is to carry (xp().aux, .sink, .link).
	bool readSecondDpSink(uint8_t aux);
	bool pickSecondLink(uint32_t pixelClockKHz);
	bool relinkSecondPipe(uint32_t pixelClockKHz);
	bool trainSecondLink();
	// The pixel-rate DTO an OTG runs on; false if its pixel clock is not that DTO.
	bool readDpDto(uint8_t otg, ModeSet::DpDto &out, uint32_t *cntl = nullptr);
	void surveySteps(const ModeSet::Step *steps, size_t count, const Dmub::Cmd *cmds);
	bool waitFrames(uint32_t frames, uint32_t otgOffset);

	bool initHardwareCursor();
	void freeHardwareCursor();
	bool hwCursorRequested { false };
	bool ndrvCursorRequested { false };   // rdna4-cursor=1: the plane for IONDRV's framebuffers too (else only the paravirtual displays')
	IOReturn loadCursorStage(uint32_t w, uint32_t h, uint32_t hotX, uint32_t hotY);   // cursorStage into the sprite and the plane
	bool hwCursorReady { false };
	bool hwCursorSet { false };
	bool hwCursorVisible { false };
	int32_t hwCursorX { 0 }, hwCursorY { 0 };
	uint16_t hwCursorHot[Pipe::kMaxOtg][2] {};   // each pipe's hot spot (x, y): the paravirtual displays' images differ (1x, 2x)
	uint32_t cursorDrawCalls { 0 }, cursorDrawLogs { 0 };
	uint32_t *cursorStage { nullptr };
	volatile uint32_t *cursorVram { nullptr };
	IODeviceMemory *cursorMemory { nullptr };
	IOMemoryMap *cursorMap { nullptr };
	uint64_t cursorMcAddr { 0 };
	uint32_t cursorWidth { 0 }, cursorHeight { 0 };
	uint32_t cursorCtlBase { 0 };                // HUBP CURSOR_CONTROL without the enable bit
	// The pipe the cursor calls program: the device's own (0xff), or an extra head's for the paravirtual display on it
	// (PvScanout::cursorImage / cursorMove, under ExtraHold); the sprite is shared, the enable bits are each pipe's.
	uint8_t cursorPipe { 0xff };
	bool cursorEnabledHw[Pipe::kMaxOtg] {};      // the enable bits as last written to each pipe's cursor
	uint32_t cursorIndex() const { return cursorPipe < Pipe::kMaxOtg ? cursorPipe : pipe.hubp < Pipe::kMaxOtg ? pipe.hubp : 0; }
	uint32_t cursorHubpOff() const { return cursorPipe < Pipe::kMaxOtg ? cursorPipe * Pipe::Reg::kHubpStride : hubpOff(); }
	uint32_t cursorDppOff() const { return cursorPipe < Pipe::kMaxOtg ? cursorPipe * 0x16bu : dppOff(); }
	uint32_t cursorOpp() const { return cursorPipe < Pipe::kMaxOtg ? cursorPipe : pipe.opp < Pipe::kMaxOtg ? pipe.opp : 0; }
	volatile uint32_t *cursorSlot() const;   // the sprite of that pipe in VRAM (cursor.cpp: one slot a pipe), and its MC address
	uint64_t cursorSlotMc() const;
	uint32_t cursorVisChanges { 0 };
	// Evidence that survives the kernel log wrapping: registry property
	// AMDRadeonRDNA4,Cursor (cursor.cpp cursorNote).
	char cursorTrail[16384] { 0 };   // W40: 16 KiB, the Linux diff and the CM/MPCC lines came on top of the 12 KiB trail
	uint16_t cursorTrailLen { 0 };
	bool cursorTrailFull { false };
	void cursorNote(const char *fmt, ...) __printflike(2, 3);
	void cursorDumpState(const char *why);
	void latchNote(const char *fmt, ...) __printflike(2, 3);   // "latch:" line, also into the cursor trail
	void cursorTrailAppend(const char *line);
	void cursorTrailAppendLocked(const char *line);
	// The MPC cursor lock (CUR_VUPDATE_LOCK_SET<opp>, dc/mpc/dcn10/dcn10_mpc.c:458-463) that
	// brackets every cursor update in amdgpu; nested calls are counted.
	void cursorMpcLock(bool lock);
	void cursorLockNote(const char *why);
	bool cursorWaitLatched(const char *why, uint32_t maxMs);
	uint32_t cursorLockDepth { 0 }, cursorLatchLogs { 0 };
	bool cursorUseLock { true };   // rdna4-cursorlock=0 turns the lock handling off (A/B control)
	bool cursorGopHeld { false };  // the lock was found held at arming; released at the end of the first bracket
	void cursorProgramPlane(bool enable);
	void cursorSelfTest();
	// W32: cursor request scheduling (DCN_CUR0_TTU_CNTL0/1). cursorRegProbe only reads and is callable from
	// the compute thread (flip hook); cursorProgramTtu writes, only inside the rdna4-cursor=2 lock bracket.
	bool cursorProgramTtu();            // opt-in: rdna4-cursorttu=1
	bool cursorProgramMissionMode();    // W34: HUBPREQ_DEBUG_DB = 1 << 8 as amdgpu does (rdna4-cursordlg=0 skips)
	// W38 (premetal/rootcause-cursor.md), rdna4-cursor=2 only:
	void cursorDsclDump(const char *why);   // read-only: DSCL, HUBP request and MPCC lock state the GOP left
	void cursorPipeFixes();                 // CRQ_EXPANSION_MODE, cursor memory power, (rdna4-cursormpcsel=1) MPCC_UPDATE_LOCK_SEL
	void cursorDsclDecide();                // DSCL_MODE/RECOUT verdict; rdna4-cursordscl=1 writes amdgpu's mode-0 set
	bool cursorWaitFrames(uint32_t n);
	const char *cursorCrcCheck(const char *label);   // OTG CRC over the square's window, cursor on/off/on/off: "YES"/"NO"/"INCONCLUSIVE"
	// W40 (premetal/verify-cursor.md): the DPP colour-management block
	void cursorCmDump(const char *why);     // read-only: CM0_CM_CONTROL and the CM sub-blocks
	void cursorDlgDump(const char *why);    // read-only: the DLG/TTU registers 0x063b-0x0655
	bool cursorCmApply();                   // CM_BYPASS = 0 + amdgpu's SDR identity CM state, OTG-locked (default on; rdna4-cursorcm=0 skips)
	void cursorCmAuto(const char *why);     // W45: cursorCmApply at arming in the normal cursor mode (rdna4-cursor=1)
	bool cursorMpccApply();                 // rdna4-cursormpcc=1: MPCC_MODE TOP_LAYER_ONLY, ALPHA_MULTIPLIED 0, as the Linux capture
	void cursorLinuxDiff(const char *why);  // read-only: static pipe registers that differ from the Linux capture
	bool cursorOtgUpdateLock(bool lock);    // the modeset OTG update-lock bracket, bounded
	bool cursorPipeFixesOn { false };
	uint32_t cursorProbeLogs { 0 };
	bool cursorProbedSet { false }, cursorProbedDraw { false };
	IOLock *cursorTrailLock { nullptr };   // the trail is appended from the display and the compute thread
	uint32_t cursorDstXOffset(uint32_t px) const;
	uint32_t cursorRefClkKHz { 50000 };          // DCHUB refclk for CURSOR_DST_X_OFFSET
	bool cursorHold { false };                   // rdna4-cursor=2: keep the test square, ignore macOS's cursor calls
	uint32_t cursorHeldCalls { 0 };
};

#endif /* RDNA4Device_hpp */
