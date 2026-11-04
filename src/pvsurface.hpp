/*
 * pvsurface.hpp
 * AMDRadeonRDNA4
 *
 * The IOSurfaces a paravirtual client renders into: a CAMetalLayer's
 * drawables reach the host as heap entries of kind 5 that name the
 * surface by its ID, with no memory of their own (docs/m0-pvgpu.md). The
 * host writes the surface through its memory descriptor.
 *
 * The lookup is the one IOSurfaceRoot::find_surface does, without its
 * check that the asking task owns the surface (the host is the kernel):
 * the root's table of surfaces by ID, under the root's lock. The offsets
 * are measured on Sequoia 24H232 and used there only; every entry is
 * checked to be an IOSurface with that ID before it is used.
 */

#ifndef PvSurface_hpp
#define PvSurface_hpp

#include <stdint.h>

namespace PvSurface
{

/* At plugin start: resolve the IOSurface kext's methods when it loads. */
void start();

/* Every row of `width` pixels of the first `height` rows of surface `id`, filled with `pixel` (4 bytes). */
bool fill(uint32_t id, uint32_t width, uint32_t height, uint32_t pixel);

} /* namespace PvSurface */

#endif /* PvSurface_hpp */
