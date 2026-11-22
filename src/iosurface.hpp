//
//  iosurface.hpp
//  AMDRadeonRDNA4
//
//  The IOSurface kext's functions, solved when it loads (plugin.cpp): the paravirtual host's type-5 textures and frames (pvgpu.cpp) and our
//  own accelerator's display pipes (rdna4accel.cpp). All null until solved, and none are if one is missing.
//

#ifndef RDNA4IOSurface_hpp
#define RDNA4IOSurface_hpp

#include <IOKit/IOMemoryDescriptor.h>

// In this order (plugin.cpp's names); methods take the object first.
struct IOSurfaceFunctions {
	void *(*lookup)(void *root, uint32_t id, task_t task);   // IOSurfaceRoot::lookupSurface(unsigned int, task*)
	IOMemoryDescriptor *(*memory)(const void *surface);      // IOSurface::getMemoryDescriptor() const
	uint32_t (*bytesPerRow)(const void *), (*width)(const void *), (*height)(const void *), (*pixelFormat)(const void *), (*baseOffset)(const void *),
	    (*surfaceId)(const void *);
	uint32_t (*planeCount)(const void *);   // the planes: IOSurface::getPlaneCount() const, getPlaneOffset/BytesPerRow/Width/Height(unsigned) const
	uint64_t (*planeOffset)(const void *, uint32_t);
	uint32_t (*planeBytesPerRow)(const void *, uint32_t), (*planeWidth)(const void *, uint32_t), (*planeHeight)(const void *, uint32_t);
	uint32_t (*lockSeed)(const void *), (*unlockSeed)(const void *);   // IOSurface::getLockSeed() const, getUnlockSeed() const
};
extern IOSurfaceFunctions gSurfaceFns;

constexpr unsigned kSurfaceFunctions = 15;
static_assert(sizeof(IOSurfaceFunctions) == kSurfaceFunctions * sizeof(void *), "one pointer per solved function");
void setSurfaceFunctions(const mach_vm_address_t fn[kSurfaceFunctions]);
// An IOSurface's ID (IOSurface::getSurfaceID), 0 when not known.
uint32_t surfaceIdOf(const void *surface);

#endif /* RDNA4IOSurface_hpp */
