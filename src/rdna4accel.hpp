//
//  rdna4accel.hpp — our own IOGraphicsAccelerator2 (docs/native-driver-plan.md, option D: derived at run time).
//
#pragma once

class IORegistryEntry;

namespace RDNA4Accel {
// Registers for IOAcceleratorFamily2's load (Lilu): its functions solved into the thunks (src/ioaccel_thunks.S), the vtable checked
// against the KDK's, the sysctl debug.rdna4_accel registered. Nothing is published until the sysctl is written.
void start();
// WindowServer's first open of the GPU wrangler: with rdna4-accel=4, waits for the accelerator (rdna4accel.cpp)
void waitForWindowServer();
// Our accelerator serves WindowServer (rdna4-accel=4): our IONDRV framebuffers without a display are left out of user-space matching
// (pvhooks.cpp wrapMatchInternal), or WindowServer makes displays of them that it composites in software (CompositorSW), whose
// Core Animation flatten path asserts ("transient surface not found", 2026-11-20)
bool hidesBareFramebuffers();
// Whether a display is on framebuffer `fb` (an IODisplayConnect under it)
bool hasDisplay(IORegistryEntry *fb);
}
