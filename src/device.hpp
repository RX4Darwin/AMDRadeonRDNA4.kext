//
//  device.hpp
//  RDNA4FB
//
//  Hardware side of RDNA4FB for one GPU: the GOP-programmed scanout, BAR5
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
#include <pexpert/pexpert.h>

#include "atombios.hpp"
#include "dmub.hpp"
#include "ipdiscovery.hpp"
#include "modes.hpp"
#include "modeset.hpp"
#include "ndrv.hpp"
#include "pipe.hpp"
#include "pipe2.hpp"

class IOMemoryMap;

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
	uint32_t            fbRowBytes { 0 };
	uint32_t            fbDepth    { 32 };

	// EDID of the boot display's sink: base block plus one CTA extension if
	// present. 0 length = no DDC sink found.
	uint8_t edidData[256] {};
	size_t  edidLen { 0 };
	// The next sink that answered on another connector, for the second head
	// (rdna4-head2, plugin.cpp): base block plus one extension. 0 length = none.
	uint8_t edid2Data[256] {};
	size_t  edid2Len { 0 };

	// Display mode table (modes.hpp). By default it holds only the boot mode;
	// with boot-arg "rdna4-modeset=1" it also carries the sink's EDID modes.
	// Modes never exceed the GOP framebuffer, and every mode keeps the boot
	// surface's pitch and memory, so a mode change only moves the timing
	// and viewport — the scanout surface itself never moves. modeCount 0 =
	// the live timing is unknown and the display is left to the boot NDRV.
	Modes::Mode modeTable[Modes::MaxModes] {};
	size_t      modeCount { 0 };
	uint32_t    currentModeId { 0 };   // Modes::Mode::id of the live mode
	uint32_t    defaultModeId { 0 };
	bool        modesetRequested { false };
	// Physical image size from the EDID. 0 = unknown.
	uint32_t    imageWidthMm { 0 }, imageHeightMm { 0 };
	const Modes::Mode *findMode(uint32_t id) const;
	// Program mode `m` on the lit HDMI pipe (modeset.hpp). On failure the
	// previous timing is programmed back and an error returned.
	IOReturn applyMode(const Modes::Mode &m);

	// The second pipe (pipe2.hpp), for a head whose surface is at CPU
	// address `surfacePhys` in the console's memory range. `level` is
	// rdna4-head2: 2 writes nothing and publishes what every step would do,
	// 3 lights the stream with a solid colour (and publishes what the plane
	// would then do), 4 also the plane.
	bool lightSecondPipe(uint32_t level, IOPhysicalAddress64 surfacePhys);
	// Read-only: the colour-path registers of the pipe the plan lit next to
	// the firmware's pipe, where they differ ("pipe2: colour:" lines).
	void comparePipeColour(const Pipe2::Plan &plan);
	// Display sleep and wake of the pipe lightSecondPipe lit, as amdgpu does
	// DPMS off and on for an HDMI stream (Pipe2::Part): the sink loses the
	// signal and sleeps; the timing generator and the plane keep running.
	void setSecondPipePower(bool on);
	// A mode switch on that pipe: the mode-set engine (modeset.hpp) pointed at
	// its blocks, with what else changes with the mode (Pipe2::modeSteps),
	// then the AVI infoframe for the new timing. The surface and its pitch
	// stay; modes are held to secondPipeModeLimits() and secondPipeModeOk().
	IOReturn applySecondPipeMode(const Modes::Mode &m);
	Modes::Limits secondPipeModeLimits() const;
	// Whether the pipe can be switched to `t` (Pipe2::modeSteps): Linux's
	// values for it are in the table or it is near the lit mode's pixel
	// clock, and the DET buffer has the segments it needs.
	bool secondPipeModeOk(const Edid::DetailedTiming &t, const char **why = nullptr);
	Pipe2::Target pipe2Target {};          // .now: the timing the pipe runs
	uint16_t pipe2ConnectorObjId { 0 };
	bool pipe2Lit { false }, pipe2On { false };
	bool runSecondPipePart(Pipe2::Part part, size_t *steps = nullptr);

	// Display power (DPMS). Off = disable the DP video stream and put the
	// sink in D3 via DPCD SET_POWER, or on an HDMI pipe blank to black via
	// the display pattern generator; on reverses it. "rdna4-nosleep=1"
	// makes it a no-op.
	void setDisplayPower(bool on);
	bool displayPowerOn { true };
	// The last 16 display power events (macOS display sleep / wake as the kext handled them), kept in the registry property RDNA4FB,DisplayPower
	// (" ## " separated, newest last): the kernel log wraps, the registry does not. One small string is rebuilt per event (events are rare).
	void displayPowerNote(bool on, const char *path, const char *result);
	char     displayPowerHist[16][72] {};
	uint32_t displayPowerEvents { 0 };

	// NDRV hardware-cursor csc backend. Cursor memory and register programming
	// are enabled only by rdna4-cursor=1; the default remains software cursor.
	bool supportsHardwareCursor() const { return hwCursorReady; }
	IOReturn setHardwareCursor(void *cursorRef);
	IOReturn drawHardwareCursor(int32_t x, int32_t y, uint32_t visible);
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
	bool copyVBIOSFromProperty();
	bool copyVBIOSFromExpansionROM();
	void publishVBIOSInfo();
	void freeVBIOS();

	// Register bases from the IP discovery binary (full flash dump injected
	// as ATY,bin_image, or the PSP's on-die copy).
	IpDiscovery ipDiscovery;
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
	void dumpDCN();

	// DP AUX software engine and DC_I2C hardware engine (EDID).
	uint32_t auxDword(uint8_t inst, uint32_t reg) const;
	int auxTransaction(uint8_t inst, uint8_t action, uint32_t address,
	                   const uint8_t *data, uint8_t len,
	                   uint8_t *reply, uint8_t replyCap, uint8_t *replyBytes);
	bool readEDID(uint8_t inst, uint8_t *edid, size_t count, uint8_t start);
	bool readEDIDI2C(uint8_t line, uint8_t *edid, size_t count, uint8_t start);
	bool i2cTransfer(uint8_t line, uint8_t addr, const uint8_t *wr, size_t wlen, uint8_t *rd, size_t rlen);
	void scdcConfigure(uint8_t line, uint8_t tmdsConfig);
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
	ModeSet::Plan modePlan {};
	bool pathForHpd(uint8_t hpdPin, AtomBios::DisplayPath &out, uint8_t *ddcLine = nullptr);
	bool runPlan(const ModeSet::Plan &plan) {
		return runSteps(plan.steps, plan.count, plan.cmds, plan.ncmds, "modeset", otgOff());
	}
	// `otgOffset`: the timing generator whose frames an Op::WaitFrames counts.
	bool runSteps(const ModeSet::Step *steps, size_t count, const Dmub::Cmd *cmds, size_t ncmds,
	              const char *tag, uint32_t otgOffset);
	void surveySteps(const ModeSet::Step *steps, size_t count, const Dmub::Cmd *cmds);
	bool waitFrames(uint32_t frames, uint32_t otgOffset);

	bool initHardwareCursor();
	void freeHardwareCursor();
	bool hwCursorRequested { false };
	bool hwCursorReady { false };
	bool hwCursorSet { false };
	bool hwCursorVisible { false };
	int32_t hwCursorX { 0 }, hwCursorY { 0 };
	uint16_t hwCursorHotX { 0 }, hwCursorHotY { 0 };
	uint32_t cursorDrawCalls { 0 }, cursorDrawLogs { 0 };
	uint32_t *cursorStage { nullptr };
	volatile uint32_t *cursorVram { nullptr };
	IODeviceMemory *cursorMemory { nullptr };
	IOMemoryMap *cursorMap { nullptr };
	uint64_t cursorMcAddr { 0 };
	uint32_t cursorWidth { 0 }, cursorHeight { 0 };
	uint32_t cursorCtlBase { 0 };                // HUBP CURSOR_CONTROL without the enable bit
	bool hwCursorEnabledHw { false };            // the enable bits as last written to the hardware
	uint32_t cursorVisChanges { 0 };
	// Evidence that survives the kernel log wrapping: registry property
	// RDNA4FB,Cursor (cursor.cpp cursorNote).
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
