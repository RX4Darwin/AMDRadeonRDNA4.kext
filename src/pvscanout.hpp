/*
 * pvscanout.hpp
 * AMDRadeonRDNA4
 *
 * The real heads as the paravirtual GPU's displays (PVGPU builds,
 * rdna4-pvgpu-disp bit 2): WindowServer composites them with Metal and
 * presents through the paravirtual display pipes, which only IOPresentment
 * drives, and IOPresentment takes a display pipe for every display
 * (docs/m0-pvgpu.md). So in such a boot the framebuffers report no display
 * and macOS sees only the pipes; each pipe's frames, SubmitTransaction's
 * IOSurface, are copied into its head's scanout surface.
 *
 * Port 0 is the boot head, ports 1.. the extra heads (rdna4-heads).
 */

#ifndef PvScanout_hpp
#define PvScanout_hpp

#include <stddef.h>
#include <stdint.h>

namespace PvScanout
{

/* A head's scanout surface: 32 bits per pixel. */
struct Head {
	uint64_t phys;          /* physical address (the VRAM aperture) */
	uint32_t pitch;         /* bytes per row */
	uint32_t width, height;
};

/* At plugin start: debug.rdna4_pvheads, -1 until decided() (rdna4-iop switches IOPresentment by it). */
void start();

/* Whether the heads are the paravirtual displays: told once the boot head's framebuffer is attached. */
void decided(bool on);

/* Whether the heads are the paravirtual displays this boot (plugin.cpp). */
bool enabled();

/* Whether decided() has been told yet. */
bool known();

/*
 * The heads with a display (plugin.cpp): the boot head and the extra heads
 * that are lit or wait to be. Fixed at the first call, which numbers the
 * ports: the paravirtual GPU has one display a port.
 */
uint32_t count();

/* The mode head `port` is lit in, or will be once it can (plugin.cpp). */
bool mode(uint32_t port, uint32_t &width, uint32_t &height);

/* The EDID of what is on head `port` (plugin.cpp); false when there is none. */
bool edid(uint32_t port, const uint8_t *&data, size_t &len);

/* The scanout of head `port` (plugin.cpp); false while it is not lit. */
bool head(uint32_t port, Head &out);

/*
 * SubmitTransaction {u32 port, u32 surface, u32 task}: the surface, an
 * IOSurface ID, into head `port`'s scanout (pvscanout.cpp).
 */
bool present(uint32_t port, uint32_t surface, uint32_t task);

/*
 * At most `height` rows of at most `width` 4-byte pixels of IOSurface `sid`
 * into `dst`, `pitch` bytes a row: as much as both have (the host,
 * pvgpu.cpp).
 */
bool copySurface(uint32_t sid, uint8_t *dst, uint32_t pitch, uint32_t width, uint32_t height);

} /* namespace PvScanout */

#endif /* PvScanout_hpp */
