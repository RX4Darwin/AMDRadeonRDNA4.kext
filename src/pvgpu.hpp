//
//  pvgpu.hpp
//  RDNA4FB
//
//  M0 of docs/metal-phase-plan.md (route B', docs/metal-spike.md s.6.1 and s.9.2, hub-task-427): the fake Apple paravirtual GPU.
//
//  Apple's own AppleParavirtGPU.kext (full macOS install only; PCI match 0xEEEE106B = vendor 0x106b, device 0xeeee) is a driver for a virtual
//  device made of an MMIO window, a FIFO in guest RAM and a version handshake. With the boot-arg rdna4-pvgpu=1 this file publishes a nub that
//  LOOKS like that PCI function to Apple's driver: an IOPCIDevice-class object with vendor/device 106b:eeee, a RAM-backed BAR0 (16 KiB, control block at
//  +0x1000), a small config space with an MSI capability, and an interrupt source that the host side can raise. A polling "host" watches the control
//  block, maps the FIFO the guest driver announces and consumes it (logging the bytes, executing nothing: M0 is "does Apple's kext accept the
//  device", the host loop proper is M1).
//
//  rdna4-pvgpu=2 additionally runs a self-test that plays the guest driver against the nub (same register sequence as AppleParavirtAccelerator:
//  map BAR0, version handshake, FIFO allocation, write, interrupt), so the nub itself can be verified in the emulated Recovery without Apple's kext.
//
//  Default off. Never in a real-card boot: a second "display adapter" with no modes must not appear next to the card's display (docs/m0-pvgpu.md).
//
//  Why a subclass of IOPCIDevice and not a Lilu patch: the personality matches IOProviderClass IOPCIDevice, the driver casts its provider with
//  OSDynamicCast<IOPCIDevice> and calls IOPCIDevice virtuals on it (mapDeviceMemoryWithRegister, findPCICapability, setMemoryEnable). IOPCIFamily is in
//  the boot collection OpenCore injects into, so unlike IOGraphicsFamily (accelcensus.hpp) it can be linked normally.
//

#ifndef RDNA4PvGpu_hpp
#define RDNA4PvGpu_hpp

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace pvgpu {
constexpr uint16_t kVendor = 0x106b;
constexpr uint16_t kDevice = 0xeeee;
constexpr uint32_t kBar0Size = 0x4000;       // reims-vgpu GFX_MMIO_SIZE; Apple's driver maps register 0x10 and works at +0x1000
constexpr uint32_t kCtrl = 0x1000;           // control block inside BAR0
// Control block registers (offsets from BAR0 + 0x1000), as used by AppleParavirtAccelerator / AppleParavirtGPUControl [MEASURED in the kext, s.6.1]:
constexpr uint32_t kRegControlFifo = 0x000;  // written 1 by setupFIFO: FIFO enabled
constexpr uint32_t kRegFifoLength = 0x004;   // total FIFO bytes (0x10000)
constexpr uint32_t kRegFifoWritten = 0x008;  // guest write counter (bytes, monotonic), updated after every writeFifo
constexpr uint32_t kRegFifoRead = 0x00c;     // host read counter: writeFifo spins until (read + ring - written) >= command length
constexpr uint32_t kRegFifoStart = 0x010;    // ring start offset inside the FIFO buffer (0x1000); the first 4 KiB are the root header
constexpr uint32_t kRegRootPage = 0x01c;     // physical page number of the 4 KiB root page
constexpr uint32_t kRegFifoBasePage = 0x030; // physical page number of the 64 KiB FIFO buffer
constexpr uint32_t kRegVersion = 0x034;      // driver writes 6 and reads back; the host chooses the version by what it returns
constexpr uint32_t kRegNumDisplays = 0x22c;  // read once by AppleParavirtGPUControl::start; 0 means one, more than 8 is clamped
} // namespace pvgpu

class RDNA4PvNub : public IOPCIDevice {
	OSDeclareDefaultStructors(RDNA4PvNub)
public:
	// Publish the nub under `parent` (any registered service; the GPU's IOPCIDevice) and start the polling host. level: 1 = nub + host, 2 = + self-test.
	// Once per boot; returns false (and logs) when it could not be built.
	static bool publish(IOService *parent, uint32_t level);
	// For machines where the GPU path never runs (a VM without the emulated RDNA4 device): if nothing published the nub within `delayMs`, publish it under
	// the platform expert. Starts a work loop and a timer; call once from plugin start.
	static void publishLater(uint32_t level, uint32_t delayMs);

	// Host side: the control block of BAR0 as dwords, valid while the nub lives (the nub is never released once published).
	volatile uint32_t *ctrl() const { return mBar0 ? reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(mBar0) + pvgpu::kCtrl) : nullptr; }
	// Run the registered handler of interrupt `source`, as the interrupt controller would. false when none is registered and enabled.
	bool raiseInterrupt(int source);
	bool configTest();      // used by the self-test: config space, capabilities, matching

	bool init(OSDictionary *propTable) APPLE_KEXT_OVERRIDE;
	bool init(IORegistryEntry *from, const IORegistryPlane *inPlane) APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;
	bool attach(IOService *provider) APPLE_KEXT_OVERRIDE;
	void detach(IOService *provider) APPLE_KEXT_OVERRIDE;
	void detachAbove(const IORegistryPlane *plane) APPLE_KEXT_OVERRIDE;
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
	                       IOUserClient **handler) APPLE_KEXT_OVERRIDE;
	bool handleOpen(IOService *forClient, IOOptionBits options, void *arg) APPLE_KEXT_OVERRIDE;
	void handleClose(IOService *forClient, IOOptionBits options) APPLE_KEXT_OVERRIDE;
	IOReturn requestProbe(IOOptionBits options) APPLE_KEXT_OVERRIDE;
	IOReturn powerStateWillChangeTo(IOPMPowerFlags capabilities, unsigned long stateNumber, IOService *whatDevice) APPLE_KEXT_OVERRIDE;
	IOReturn setPowerState(unsigned long state, IOService *device) APPLE_KEXT_OVERRIDE;
	unsigned long maxCapabilityForDomainState(IOPMPowerFlags domainState) APPLE_KEXT_OVERRIDE;
	unsigned long initialPowerStateForDomainState(IOPMPowerFlags domainState) APPLE_KEXT_OVERRIDE;
	unsigned long powerStateForDomainState(IOPMPowerFlags domainState) APPLE_KEXT_OVERRIDE;
	bool compareName(OSString *name, OSString **matched = nullptr) const APPLE_KEXT_OVERRIDE;
	bool matchPropertyTable(OSDictionary *table) APPLE_KEXT_OVERRIDE;
	bool matchPropertyTable(OSDictionary *table, SInt32 *score) APPLE_KEXT_OVERRIDE;
	IOService *matchLocation(IOService *client) APPLE_KEXT_OVERRIDE;
	IOReturn getResources() APPLE_KEXT_OVERRIDE;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *p1, void *p2, void *p3, void *p4) APPLE_KEXT_OVERRIDE;
	IOReturn callPlatformFunction(const char *functionName, bool waitForFunction, void *p1, void *p2, void *p3, void *p4) APPLE_KEXT_OVERRIDE;
	IODeviceMemory *getDeviceMemoryWithIndex(unsigned int index) APPLE_KEXT_OVERRIDE;

	UInt32 configRead32(IOPCIAddressSpace space, UInt8 offset) APPLE_KEXT_OVERRIDE;
	void configWrite32(IOPCIAddressSpace space, UInt8 offset, UInt32 data) APPLE_KEXT_OVERRIDE;
	UInt16 configRead16(IOPCIAddressSpace space, UInt8 offset) APPLE_KEXT_OVERRIDE;
	void configWrite16(IOPCIAddressSpace space, UInt8 offset, UInt16 data) APPLE_KEXT_OVERRIDE;
	UInt8 configRead8(IOPCIAddressSpace space, UInt8 offset) APPLE_KEXT_OVERRIDE;
	void configWrite8(IOPCIAddressSpace space, UInt8 offset, UInt8 data) APPLE_KEXT_OVERRIDE;
	IOReturn saveDeviceState(IOOptionBits options = 0) APPLE_KEXT_OVERRIDE;
	IOReturn restoreDeviceState(IOOptionBits options = 0) APPLE_KEXT_OVERRIDE;
	UInt32 setConfigBits(UInt8 offset, UInt32 mask, UInt32 value) APPLE_KEXT_OVERRIDE;
	bool setMemoryEnable(bool enable) APPLE_KEXT_OVERRIDE;
	bool setIOEnable(bool enable, bool exclusive = false) APPLE_KEXT_OVERRIDE;
	bool setBusMasterEnable(bool enable) APPLE_KEXT_OVERRIDE;
	UInt32 findPCICapability(UInt8 capabilityID, UInt8 *offset = nullptr) APPLE_KEXT_OVERRIDE;
	UInt8 getBusNumber() APPLE_KEXT_OVERRIDE;
	UInt8 getDeviceNumber() APPLE_KEXT_OVERRIDE;
	UInt8 getFunctionNumber() APPLE_KEXT_OVERRIDE;
	IODeviceMemory *getDeviceMemoryWithRegister(UInt8 reg) APPLE_KEXT_OVERRIDE;
	IOMemoryMap *mapDeviceMemoryWithRegister(UInt8 reg, IOOptionBits options = 0) APPLE_KEXT_OVERRIDE;
	IODeviceMemory *ioDeviceMemory() APPLE_KEXT_OVERRIDE;
	void ioWrite32(UInt16 offset, UInt32 value, IOMemoryMap *map = nullptr) APPLE_KEXT_OVERRIDE;
	void ioWrite16(UInt16 offset, UInt16 value, IOMemoryMap *map = nullptr) APPLE_KEXT_OVERRIDE;
	void ioWrite8(UInt16 offset, UInt8 value, IOMemoryMap *map = nullptr) APPLE_KEXT_OVERRIDE;
	UInt32 ioRead32(UInt16 offset, IOMemoryMap *map = nullptr) APPLE_KEXT_OVERRIDE;
	UInt16 ioRead16(UInt16 offset, IOMemoryMap *map = nullptr) APPLE_KEXT_OVERRIDE;
	UInt8 ioRead8(UInt16 offset, IOMemoryMap *map = nullptr) APPLE_KEXT_OVERRIDE;
	bool hasPCIPowerManagement(IOOptionBits state = 0) APPLE_KEXT_OVERRIDE;
	IOReturn enablePCIPowerManagement(IOOptionBits state = 0xffffffff) APPLE_KEXT_OVERRIDE;
	UInt32 extendedFindPCICapability(UInt32 capabilityID, IOByteCount *offset = nullptr) APPLE_KEXT_OVERRIDE;

	IOReturn registerInterrupt(int source, OSObject *target, IOInterruptAction handler, void *refCon = nullptr) APPLE_KEXT_OVERRIDE;
	IOReturn unregisterInterrupt(int source) APPLE_KEXT_OVERRIDE;
	IOReturn getInterruptType(int source, int *interruptType) APPLE_KEXT_OVERRIDE;
	IOReturn enableInterrupt(int source) APPLE_KEXT_OVERRIDE;
	IOReturn disableInterrupt(int source) APPLE_KEXT_OVERRIDE;
	IOReturn causeInterrupt(int source) APPLE_KEXT_OVERRIDE;

	// The header's reserved slots 3-5 and 16-21 are real virtuals in Tahoe's IOPCIFamily (the reserved symbols no longer exist there), so a vtable that
	// inherits them as references would leave undefined symbols in this kext. Overriding them here removes the references; patchVtable() then points
	// the slots that matter at working implementations (below).
	void _RESERVEDIOPCIDevice3() APPLE_KEXT_OVERRIDE {}
	void _RESERVEDIOPCIDevice4() APPLE_KEXT_OVERRIDE {}
	void _RESERVEDIOPCIDevice5() APPLE_KEXT_OVERRIDE {}
	void _RESERVEDIOPCIDevice16() APPLE_KEXT_OVERRIDE {}
	void _RESERVEDIOPCIDevice17() APPLE_KEXT_OVERRIDE {}
	void _RESERVEDIOPCIDevice18() APPLE_KEXT_OVERRIDE {}
	void _RESERVEDIOPCIDevice19() APPLE_KEXT_OVERRIDE {}
	void _RESERVEDIOPCIDevice20() APPLE_KEXT_OVERRIDE {}
	void _RESERVEDIOPCIDevice21() APPLE_KEXT_OVERRIDE {}

	// The slots Tahoe's IOPCIDevice has beyond MacKernelSDK's header (the header calls them reserved): see patchVtable() and tools/m0/check-vtable.py.
	UInt32 tahoeConfigRead(UInt32 width, UInt8 offset);
	void tahoeConfigWrite(UInt32 width, UInt8 offset, UInt32 data);
	IOReturn tahoeDeviceMemoryRead(UInt8 bar, UInt64 offset, void *data, UInt8 size);
	IOReturn tahoeDeviceMemoryWrite(UInt8 bar, UInt64 offset, UInt64 data, UInt8 size);

private:
	bool build();
	void patchVtable();

	static constexpr int kMaxInterrupts = 4;
	struct Interrupt {
		OSObject *target { nullptr };
		IOInterruptAction handler { nullptr };
		void *refCon { nullptr };
		bool registered { false };
		bool enabled { false };
	};
	IOBufferMemoryDescriptor *mBar0Desc { nullptr };
	void *mBar0 { nullptr };                         // kernel VA of the BAR0 RAM (the buffer's own mapping; Apple's driver gets an alias)
	uint8_t mConfig[256] {};
	Interrupt mInt[kMaxInterrupts] {};
	IOLock *mLock { nullptr };
};

#endif /* RDNA4PvGpu_hpp */
