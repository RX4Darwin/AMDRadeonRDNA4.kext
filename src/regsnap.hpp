/*
 * regsnap.hpp
 * AMDRadeonRDNA4
 *
 * A snapshot of the registers a plan wrote, to compare later: what lighting
 * left in them against what a display sleep and wake leave. A wake that
 * leaves the display dark differs somewhere (rdna4-wakediff=1).
 * Freestanding apart from the read callback.
 */

#ifndef RegSnap_hpp
#define RegSnap_hpp

#include <stddef.h>
#include <stdint.h>

#include "modeset.hpp"

namespace RegSnap
{

constexpr size_t kMaxRegs = 512;

struct Reg {
	uint8_t seg;
	uint32_t dword;
	uint32_t value;
	const char *what;
};

struct Snap {
	Reg regs[kMaxRegs];
	size_t count;
};

typedef uint32_t (*ReadFn)(void *ctx, uint8_t seg, uint32_t dword);
typedef void (*DiffFn)(void *ctx, const Reg &was, uint32_t now);

/* The registers the plan's writes touch, once each, with their values now. */
void take(Snap &snap, const ModeSet::Step *steps, size_t count, ReadFn read, void *ctx);

/* Calls diff for each register whose value is not what the snapshot holds. */
size_t compare(const Snap &snap, ReadFn read, DiffFn diff, void *ctx);

} /* namespace RegSnap */

#endif /* RegSnap_hpp */
