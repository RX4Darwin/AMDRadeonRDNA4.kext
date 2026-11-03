/*
 * pvexec.cpp
 * AMDRadeonRDNA4
 *
 * See pvexec.hpp.
 */

#include "pvexec.hpp"
#include "pvrun.hpp"
#include "klog.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/libkern.h>

#define XLOG(fmt, ...) \
	rdna4Log("AMDRadeonRDNA4: pvgpu: exec: " fmt "\n", ##__VA_ARGS__)

namespace
{

struct Task {
	bool used;
	uint32_t raw;        /* the id as DefineHostTask sends it */
	uint32_t rootPfn;
	uint32_t levels;
	uint64_t space;
	uint64_t heapVa;     /* GPU address of the resource heap */
	uint32_t heapSize;
};

/*
 * ExecIndirect's payload (measured, card, Sequoia): a header, `count`
 * resource entries and the command buffer's GPU address and length.
 */
struct ExecHeader {
	uint32_t task;       /* unshifted, as CommitIntoGPUPageTable names it */
	uint32_t count;
	uint32_t flags;
};

struct ExecEntry {
	uint32_t resource;
	uint32_t usage;
	uint64_t a, b;
};

struct ExecTail {
	uint64_t va;
	uint64_t len;
};

Task gTasks[PvExec::kMaxTasks];
uint32_t gExecs;
uint32_t gWalkFails;
uint32_t gPipelines;

/* Execs whose bytes are dumped, to learn their layout. */
constexpr uint32_t kDumpedExecs = 8;
constexpr size_t kDumpBytes = 128;

uint32_t rd32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));

	return v;
}

uint64_t rd64(const uint8_t *p)
{
	uint64_t v;

	memcpy(&v, p, sizeof(v));

	return v;
}

/* The task DefineHostTask named `raw`, or nullptr. */
Task *findTask(uint32_t raw)
{
	for (Task &t : gTasks) {
		if (t.used && t.raw == raw)
			return &t;
	}

	return nullptr;
}

/* The task by the id the other commands use (DefineHostTask sends it << 1). */
Task *taskById(uint32_t id)
{
	return findTask(id << 1);
}

Task *freeSlot()
{
	for (Task &t : gTasks) {
		if (!t.used)
			return &t;
	}

	return nullptr;
}

/* `len` bytes as hex into `out`, at most `cap` characters. */
void hex(const uint8_t *p, size_t len, char *out, size_t cap)
{
	size_t at = 0;

	out[0] = 0;

	for (size_t i = 0; i < len && at + 3 < cap; i++) {
		snprintf(out + at, cap - at, "%02x", p[i]);
		at += 2;
		if ((i & 3) == 3 && at + 2 < cap)
			out[at++] = ' ';
	}

	out[at < cap ? at : cap - 1] = 0;
}

void dumpGpu(const char *what, uint32_t task, uint64_t va, size_t len)
{
	uint8_t bytes[kDumpBytes];
	char line[kDumpBytes * 3];

	if (len > sizeof(bytes))
		len = sizeof(bytes);

	if (!PvExec::readGpu(task, va, bytes, len)) {
		XLOG("%s: task %u va 0x%llx not mapped", what, task, va);
		return;
	}

	hex(bytes, len, line, sizeof(line));
	XLOG("%s: task %u va 0x%llx: %s", what, task, va, line);
}

/* Which entries of a node are set: up to 8 ranges, for a failed walk. */
void logNode(const char *what, uint64_t pa)
{
	uint32_t *e = static_cast<uint32_t *>(IOMalloc(0x1000));
	char line[200];
	size_t at = 0;
	int start = -1;

	if (!e)
		return;

	if (!PvExec::readPhys(pa, e, 0x1000)) {
		IOFree(e, 0x1000);
		return;
	}

	line[0] = 0;

	for (int i = 0; i <= 1024 && at + 40 < sizeof(line); i++) {
		const bool set = i < 1024 && e[i];

		if (set && start < 0)
			start = i;

		if (!set && start >= 0) {
			const int n = snprintf(line + at, sizeof(line) - at, " %d-%d (0x%08x)", start,
					       i - 1, e[start]);

			if (n > 0)
				at += static_cast<size_t>(n);
			start = -1;
		}
	}

	XLOG("%s at 0x%llx, set entries:%s", what, pa, at ? line : " none");
	IOFree(e, 0x1000);
}

/* The physical address of a task's GPU address, or 0. */
uint64_t translate(const Task &t, uint64_t va)
{
	uint64_t node = static_cast<uint64_t>(t.rootPfn) << 12;
	uint32_t entry = 0;

	if (va >= t.space)
		return 0;

	for (int level = static_cast<int>(t.levels) - 1; level >= 0; level--) {
		const uint32_t index = PvExec::indexAt(va, static_cast<uint32_t>(level));
		const bool read = PvExec::readPhys(node + 4ull * index, &entry, sizeof(entry));

		if (!read || !(entry & PvExec::kEntryPageMask)) {
			if (gWalkFails++ < 3) {
				XLOG("walk of va 0x%llx (%u levels, root page 0x%x): level %d index %u at 0x%llx: %s 0x%08x",
				     va, t.levels, t.rootPfn, level, index, node + 4ull * index,
				     read ? "entry" : "unreadable", entry);
				logNode("  that node", node);
				logNode("  the root", static_cast<uint64_t>(t.rootPfn) << 12);
			}
			return 0;
		}

		node = static_cast<uint64_t>(entry & PvExec::kEntryPageMask) << 12;
	}

	return node | (va & 0xfff);
}

} /* namespace */

/* Copy between guest physical memory and `buf`, one mapping per call. */
bool PvExec::accessPhys(uint64_t pa, void *buf, size_t len, bool write)
{
	const IOOptionBits how = kIOMapAnywhere | (write ? 0 : kIOMapReadOnly);
	IOMemoryDescriptor *desc;
	IOMemoryMap *map;
	void *va;

	desc = IOMemoryDescriptor::withPhysicalAddress(static_cast<IOPhysicalAddress>(pa), len,
						       write ? kIODirectionInOut : kIODirectionIn);
	if (!desc)
		return false;

	map = desc->createMappingInTask(kernel_task, 0, how);
	if (!map) {
		desc->release();
		return false;
	}

	va = reinterpret_cast<void *>(map->getVirtualAddress());
	if (write)
		memcpy(va, buf, len);
	else
		memcpy(buf, va, len);

	map->release();
	desc->release();

	return true;
}

bool PvExec::readPhys(uint64_t pa, void *out, size_t len)
{
	return accessPhys(pa, out, len, false);
}

/* Copy between a task's GPU addresses and `buf`, page by page. */
bool PvExec::accessGpu(uint32_t task, uint64_t va, void *buf, size_t len, bool write)
{
	const Task *t = taskById(task);
	uint8_t *p = static_cast<uint8_t *>(buf);

	if (!t)
		return false;

	while (len) {
		const size_t inPage = 0x1000 - (va & 0xfff);
		const size_t n = len < inPage ? len : inPage;
		const uint64_t pa = translate(*t, va);

		if (!pa || !accessPhys(pa, p, n, write))
			return false;

		p += n;
		va += n;
		len -= n;
	}

	return true;
}

bool PvExec::readGpu(uint32_t task, uint64_t va, void *out, size_t len)
{
	return accessGpu(task, va, out, len, false);
}

bool PvExec::writeGpu(uint32_t task, uint64_t va, const void *in, size_t len)
{
	return accessGpu(task, va, const_cast<void *>(in), len, true);
}

/*
 * A resource by the ref the stream names it with: entry `ref` of the
 * task's resource heap, {u32 kind | descriptor size << 8, u32 descriptor
 * GPU address, u32 0}, and the descriptor there. A buffer's (kind 1,
 * 16 bytes) is {u64 length, u64 GPU page number} (measured, card).
 */
bool PvExec::resolveBuffer(uint32_t task, uint32_t ref, uint64_t &va, uint64_t &len)
{
	const Task *t = taskById(task);
	uint32_t entry[3];
	uint64_t desc[2];

	if (!t || !t->heapVa || 12ull * (ref + 1) > t->heapSize)
		return false;

	if (!readGpu(task, t->heapVa + 12ull * ref, entry, sizeof(entry)))
		return false;

	if ((entry[0] & 0xff) != kResourceBuffer || (entry[0] >> 8) < sizeof(desc))
		return false;

	if (!readGpu(task, entry[1], desc, sizeof(desc)))
		return false;

	len = desc[0];
	va = desc[1] << 12;

	return true;
}

/* {u32 task << 1, u64 address-space size, u32 page-table root PFN} */
void PvExec::defineTask(const uint8_t *payload, uint32_t len)
{
	Task *t;

	if (len < 16)
		return;

	t = findTask(rd32(payload));
	if (!t)
		t = freeSlot();

	if (!t) {
		XLOG("no slot for task 0x%x", rd32(payload));
		return;
	}

	*t = {};
	t->used = true;
	t->raw = rd32(payload);
	t->space = rd64(payload + 4);
	t->rootPfn = rd32(payload + 12);
	t->levels = kPageTableLevels;
}

/* {u32 task} */
void PvExec::freeTask(const uint8_t *payload, uint32_t len)
{
	Task *t;

	if (len < 4)
		return;

	t = findTask(rd32(payload));
	if (t)
		t->used = false;
}

/* {u32 task, u32 heap GPU page, u32 size} (AppleParavirtTask::setResourceHeap) */
void PvExec::setHeap(const uint8_t *payload, uint32_t len)
{
	const uint32_t id = len >= 12 ? rd32(payload) : 0;
	Task *t = taskById(id);

	if (len < 12 || !t)
		return;

	t->heapVa = static_cast<uint64_t>(rd32(payload + 4)) << 12;
	t->heapSize = rd32(payload + 8);
}

/*
 * A ref's heap entry, its descriptor and, for each u64 of the descriptor
 * that could be a GPU page number, the start of that page.
 */
static void dumpRef(uint32_t task, uint32_t ref)
{
	const Task *t = taskById(task);
	uint32_t entry[3];
	uint64_t desc[8] = {};
	uint32_t size;

	if (!t || !t->heapVa || 12ull * (ref + 1) > t->heapSize ||
	    !PvExec::readGpu(task, t->heapVa + 12ull * ref, entry, sizeof(entry)))
		return;

	size = entry[0] >> 8;
	if (size > sizeof(desc))
		size = sizeof(desc);

	if (!PvExec::readGpu(task, entry[1], desc, size))
		return;

	XLOG("  ref %u: kind %u, %u descriptor bytes at 0x%x: 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx",
	     ref, entry[0] & 0xff, entry[0] >> 8, entry[1], desc[0], desc[1], desc[2], desc[3],
	     desc[4], desc[5]);

	for (uint32_t i = 0; i < size / 8; i++) {
		if (desc[i] >= 0x1 && desc[i] < (1ull << 22))
			dumpGpu("    page", task, desc[i] << 12, kDumpBytes);
	}
}

/*
 * CreateComputePipeline (0x3b): its payload and, for every u64 in it that
 * could be an address in the task, the bytes there (the layout is still
 * being learnt; the first u32 is taken to be the task).
 */
void PvExec::createComputePipeline(const uint8_t *payload, uint32_t len)
{
	char line[kDumpBytes * 3];
	const uint32_t task = len >= 4 ? rd32(payload) : 0;

	if (gPipelines++ >= 4)
		return;

	hex(payload, len < kDumpBytes ? len : kDumpBytes, line, sizeof(line));
	XLOG("CreateComputePipeline, %u payload bytes: %s", len, line);

	for (uint32_t off = 4; off + 4 <= len && off < 16; off += 4)
		dumpRef(task, rd32(payload + off));
}

/* The first execs, in full: their layout is still being learnt. */
static void dumpExec(const ExecHeader &h, const uint8_t *payload, const ExecTail &tail,
	      uint32_t channel)
{
	const Task *t = taskById(h.task);

	XLOG("ExecIndirect on channel %u: task %u, %u resource(s), flags 0x%x, commands at 0x%llx, %llu bytes",
	     channel, h.task, h.count, h.flags, tail.va, tail.len);

	for (uint32_t i = 0; i < h.count; i++) {
		ExecEntry e;

		memcpy(&e, payload + sizeof(h) + i * sizeof(e), sizeof(e));
		XLOG("  resource %u usage 0x%x, 0x%llx 0x%llx", e.resource, e.usage, e.a, e.b);
	}

	if (t && t->heapVa)
		dumpGpu("resource heap", h.task, t->heapVa, kDumpBytes);

	for (uint64_t off = 0; off < tail.len && off < 4 * kDumpBytes; off += kDumpBytes)
		dumpGpu("commands", h.task, tail.va + off, tail.len - off);
}

void PvExec::execIndirect(const uint8_t *payload, uint32_t len, uint32_t channel)
{
	ExecHeader h;
	ExecTail tail;
	char line[kDumpBytes * 3];
	size_t need;

	if (len < sizeof(h))
		return;

	memcpy(&h, payload, sizeof(h));
	need = sizeof(h) + h.count * sizeof(ExecEntry) + sizeof(tail);
	if (h.count > 64 || len < need) {
		hex(payload, len < kDumpBytes ? len : kDumpBytes, line, sizeof(line));
		XLOG("ExecIndirect on channel %u: %u bytes do not fit %u entries: %s", channel, len,
		     h.count, line);
		return;
	}

	memcpy(&tail, payload + need - sizeof(tail), sizeof(tail));
	if (gExecs++ < kDumpedExecs)
		dumpExec(h, payload, tail, channel);

	PvRun::run(h.task, tail.va, tail.len);
}
