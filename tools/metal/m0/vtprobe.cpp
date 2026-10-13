#include <IOKit/pci/IOPCIDevice.h>
class VtProbe : public IOPCIDevice { OSDeclareDefaultStructors(VtProbe) public: IOMemoryMap *mapDeviceMemoryWithRegister(UInt8 reg, IOOptionBits options = 0) APPLE_KEXT_OVERRIDE; };
IOMemoryMap *VtProbe::mapDeviceMemoryWithRegister(UInt8, IOOptionBits) { return nullptr; }
