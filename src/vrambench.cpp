/*
 * vrambench.cpp
 * AMDRadeonRDNA4
 *
 * How fast the CPU writes and reads VRAM through BAR0 in each cache mode,
 * for rdna4-vrambench=1 (a test boot's option). Without acceleration
 * WindowServer draws every frame with the CPU into the framebuffer, so
 * this is the speed of the desktop. Uses VRAM no head and no compute pool
 * takes (vrambench.hpp).
 */

#include "vrambench.hpp"
#include "klog.hpp"
#include "trial.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <libkern/libkern.h>

#define BLOG(fmt, ...) \
	rdna4Log("AMDRadeonRDNA4: vrambench: " fmt "\n", ##__VA_ARGS__)

namespace
{

constexpr size_t kWriteBytes = 8u << 20;
constexpr size_t kReadBytes = 1u << 20;

struct Mode {
	IOOptionBits option;
	const char *name;
};

constexpr Mode kModes[] = {
	{ kIOMapDefaultCache, "default" },
	{ kIOMapInhibitCache, "uncached" },
	{ kIOMapWriteCombineCache, "write-combined" },
};

uint64_t nowNs()
{
	uint64_t abs = mach_absolute_time();
	uint64_t ns;

	absolutetime_to_nanoseconds(abs, &ns);

	return ns;
}

/* MB/s of `bytes` in `ns`. */
uint64_t rate(size_t bytes, uint64_t ns)
{
	return ns ? bytes * 1000ull / ns : 0;
}

uint64_t writeTest(volatile uint64_t *p)
{
	const uint64_t start = nowNs();

	for (size_t i = 0; i < kWriteBytes / 8; i++)
		p[i] = i;

	return rate(kWriteBytes, nowNs() - start);
}

uint64_t readTest(volatile uint64_t *p)
{
	const uint64_t start = nowNs();
	uint64_t sum = 0;

	for (size_t i = 0; i < kReadBytes / 8; i++)
		sum += p[i];

	(void)sum;

	return rate(kReadBytes, nowNs() - start);
}

void benchMode(uint64_t pa, const Mode &m)
{
	IOMemoryDescriptor *desc;
	IOMemoryMap *map;
	volatile uint64_t *p;
	uint64_t w, r;

	desc = IOMemoryDescriptor::withPhysicalAddress(static_cast<IOPhysicalAddress>(pa),
						       kWriteBytes, kIODirectionInOut);
	if (!desc)
		return;

	map = desc->createMappingInTask(kernel_task, 0, kIOMapAnywhere | m.option);
	if (!map) {
		BLOG("%s: no mapping", m.name);
		desc->release();
		return;
	}

	p = reinterpret_cast<volatile uint64_t *>(map->getVirtualAddress());
	w = writeTest(p);
	r = readTest(p);
	BLOG("%s: write %llu MB/s, read %llu MB/s", m.name, w, r);

	map->release();
	desc->release();
}

} /* namespace */

void VramBench::run(uint64_t fbPhysBase, uint64_t freeOffset)
{
	uint32_t on = 0;

	if (!Trial::option("rdna4-vrambench", on) || !on)
		return;

	BLOG("8 MiB at VRAM+0x%llx through BAR0", freeOffset);

	for (const Mode &m : kModes)
		benchMode(fbPhysBase + freeOffset, m);
}
