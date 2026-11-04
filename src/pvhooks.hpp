/*
 * pvhooks.hpp
 * AMDRadeonRDNA4
 *
 * Hooks into Apple's accelerator family for the fake paravirtual GPU
 * (pvgpu.hpp), built with it (make PVGPU=1).
 *
 * IOGraphicsAccelerator2 disables itself when its framebuffer's mode
 * starts to change and enables itself when the change is done. Every
 * resource a Metal client makes waits for it to be enabled. Our host does
 * not finish the paravirt framebuffer's mode change yet, so the
 * accelerator stayed disabled and every client blocked in makeBuffer
 * (docs/m0-pvgpu.md). Until it does, the paravirt accelerator is kept
 * enabled, and enabled for the first client that waits for it (it starts
 * disabled): there is no hardware behind it that a mode change could hurt.
 *
 * While the heads are the paravirtual displays (pvscanout.hpp), our
 * framebuffers refuse user clients once the plugin has decided so: the
 * extra heads' framebuffers, not the boot framebuffer WindowServer opened
 * before.
 */

#ifndef PvHooks_hpp
#define PvHooks_hpp

namespace PvHooks
{

/* Once, from pluginStart: ask Lilu for IOAcceleratorFamily2 and IOGraphicsFamily. */
void start();

} /* namespace PvHooks */

#endif /* PvHooks_hpp */
