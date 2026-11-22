//
//  iosurface.cpp
//  AMDRadeonRDNA4
//
//  The IOSurface kext's functions (iosurface.hpp).
//

#include "iosurface.hpp"

IOSurfaceFunctions gSurfaceFns;

void setSurfaceFunctions(const mach_vm_address_t fn[kSurfaceFunctions]) { memcpy(&gSurfaceFns, fn, sizeof(gSurfaceFns)); }

uint32_t surfaceIdOf(const void *surface)
{
	return surface && gSurfaceFns.surfaceId ? gSurfaceFns.surfaceId(surface) : 0;
}
