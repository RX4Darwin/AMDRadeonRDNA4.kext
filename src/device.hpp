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
#include "pipe.hpp"

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
	// Program mode `m` on the lit pipe. Returns an error (and leaves the
	// pipe untouched) until the mode-set engine exists.
	IOReturn applyMode(const Modes::Mode &m);

	// Display power (DPMS). Off = disable the DP video stream and put the
	// sink in D3 via DPCD SET_POWER, or on an HDMI pipe blank to black via
	// the display pattern generator; on reverses it. "rdna4-nosleep=1"
	// makes it a no-op.
	void setDisplayPower(bool on);
	bool displayPowerOn { true };

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
	uint64_t measureFramePeriodNs();
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
};

#endif /* RDNA4Device_hpp */
