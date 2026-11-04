/*
 * pvscanout.cpp
 * AMDRadeonRDNA4
 *
 * See pvscanout.hpp. The copy runs on the CPU, the IOSurface's memory into
 * a write-combined mapping of the head's scanout, in the host's FIFO
 * thread: SubmitTransaction completes with the batch's stamp after it.
 */

#include "pvscanout.hpp"
#include "klog.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/libkern.h>
#include <sys/sysctl.h>

#define VLOG(fmt, ...) \
	rdna4Log("AMDRadeonRDNA4: pvgpu: scanout: " fmt "\n", ##__VA_ARGS__)

namespace
{

constexpr uint32_t kMaxPorts = 8;
constexpr uint32_t kLoggedFrames = 4;
constexpr uint32_t kSampleEvery = 300;   /* frames between two lines of samples */
constexpr uint32_t kFirstSample = 30;    /* and one early, before the screen settles */

/* A head's scanout as mapped, kept while its surface stays where it is. */
struct Mapping {
	PvScanout::Head head;
	IOMemoryDescriptor *desc;
	IOMemoryMap *map;
	uint32_t frames;
	uint32_t failures;
};

Mapping gMap[kMaxPorts];

/* -1 not known yet, 0 the framebuffers are macOS's displays, 1 the paravirtual pipes are */
int gHeads = -1;

SYSCTL_INT(_debug, OID_AUTO, rdna4_pvheads, CTLFLAG_RD | CTLFLAG_LOCKED, &gHeads, 0,
	   "AMDRadeonRDNA4: the heads are the paravirtual displays (-1 not known yet)");

void unmap(Mapping &m)
{
	OSSafeReleaseNULL(m.map);
	OSSafeReleaseNULL(m.desc);
}

/* The mapping of head `port`'s scanout now, made again when it moved. */
uint8_t *scanout(uint32_t port, PvScanout::Head &head)
{
	Mapping &m = gMap[port];
	const uint64_t bytes = static_cast<uint64_t>(head.pitch) * head.height;

	if (m.map && !memcmp(&m.head, &head, sizeof(head)))
		return reinterpret_cast<uint8_t *>(m.map->getVirtualAddress());

	unmap(m);
	m.head = head;
	m.desc = IOMemoryDescriptor::withPhysicalAddress(static_cast<IOPhysicalAddress>(head.phys), bytes, kIODirectionInOut);
	if (m.desc)
		m.map = m.desc->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);

	if (!m.map) {
		VLOG("port %u: cannot map the scanout at 0x%llx (%llu bytes)", port, head.phys, bytes);
		unmap(m);
		return nullptr;
	}

	VLOG("port %u: scanout 0x%llx, %ux%u, pitch %u", port, head.phys, head.width, head.height, head.pitch);

	return reinterpret_cast<uint8_t *>(m.map->getVirtualAddress());
}

/* How many frames a port has had, and some pixels of the last one: the corners' insides and the centre. */
void logSamples(uint32_t port, const uint8_t *dst, const PvScanout::Head &h)
{
	const uint32_t xs[] = { 16, h.width - 16, h.width / 2, 16, h.width - 16 };
	const uint32_t ys[] = { 16, 16, h.height / 2, h.height - 16, h.height - 16 };
	uint32_t px[5];

	for (int i = 0; i < 5; i++)
		px[i] = *reinterpret_cast<const volatile uint32_t *>(dst + static_cast<size_t>(ys[i]) * h.pitch + 4ull * xs[i]);

	VLOG("port %u: %u frames, %u failed; pixels %08x %08x centre %08x %08x %08x", port, gMap[port].frames, gMap[port].failures,
	     px[0], px[1], px[2], px[3], px[4]);
}

} /* namespace */

void PvScanout::start()
{
	sysctl_register_oid(&sysctl__debug_rdna4_pvheads);
}

void PvScanout::decided(bool on)
{
	gHeads = on;
}

bool PvScanout::present(uint32_t port, uint32_t surface, uint32_t task)
{
	PvScanout::Head h;
	uint8_t *dst;
	uint32_t id;
	bool ok;

	if (port >= kMaxPorts || !PvScanout::head(port, h))
		return false;

	dst = scanout(port, h);
	if (!dst)
		return false;

	Mapping &m = gMap[port];

	id = surface;
	ok = PvScanout::copySurface(id, dst, h.pitch, h.width, h.height);

	if (m.frames < kLoggedFrames || (!ok && m.failures < kLoggedFrames))
		VLOG("port %u: frame %u: IOSurface %u (task %u) %s", port, m.frames, id, task, ok ? "copied" : "NOT copied");

	if (ok && (m.frames % kSampleEvery == 0 || m.frames == kFirstSample))
		logSamples(port, dst, h);

	if (ok)
		m.frames++;
	else
		m.failures++;

	return ok;
}
