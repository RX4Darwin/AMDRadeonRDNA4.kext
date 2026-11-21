//
//  rdna4accel.hpp — our own IOGraphicsAccelerator2 (docs/native-driver-plan.md, option D: derived at run time).
//
#pragma once

class IORegistryEntry;
class OSDictionary;

namespace RDNA4Accel {
// Registers for IOAcceleratorFamily2's load (Lilu): its functions solved into the thunks (src/ioaccel_thunks.S), the vtable checked
// against the KDK's, the sysctl debug.rdna4_accel registered. Nothing is published until the sysctl is written.
void start();
// WindowServer's first open of the GPU wrangler: with rdna4-accel=4, waits for the accelerator (rdna4accel.cpp)
void waitForWindowServer();
// Our accelerator serves WindowServer (rdna4-accel=4): one of our IONDRV framebuffers without a display, left out of user-space matching
// (pvhooks.cpp wrapMatchInternal), or WindowServer makes displays of them that it composites in software (CompositorSW), whose
// Core Animation flatten path asserts ("transient surface not found", 2026-11-20). Decided when WindowServer is let go.
bool isBareFramebuffer(IORegistryEntry *fb);
bool framebuffersDecided();   // (rdna4-accel=4: the bare ones known)
// Whether a user-space match leaves `service` out (pvhooks.cpp wrapMatchInternal): with rdna4-accel=4, our bare framebuffers
bool hidesFromMatch(IORegistryEntry *service, OSDictionary *table);
// Whether AGDC lists framebuffer `fb` (0x980): not a bare one once decided, before that one with a display
bool showsFramebuffer(IORegistryEntry *fb);
}
