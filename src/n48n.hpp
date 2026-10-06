//
//  n48n.hpp
//  RDNA4FB
//
//  The kernel half of the interface the RADV Darwin port speaks
//  (vulkan/navi48_native_abi.h, "N48N"; docs/vulkan-port.md): one client's
//  buffers, its GPU address space, its contexts, its work and the display it
//  may show pictures on, behind the calls IOConnectCallMethod carries:
//  selectors 0 to 14. What a command buffer may be, when work counts as
//  lost and which buffer may be shown is decided here; the graphics queue
//  and the display's registers are the backend's.
//
//  The rules each call enforces are the ones Mesa's own stand-in for the
//  kext states (src/amd/common/darwin/ac_darwin_mock.c in the patched tree)
//  and Navi48-MacOS's kext follows (its native_s1c_pure.h, read as a
//  reference): exact argument counts and sizes, what a buffer may be, where a
//  mapping may sit, which flags mean what in a page-table entry.
//
//  Freestanding: memory and registers come through Backend, so the same code
//  runs in the kext, in tools/atomdump.cpp's checks, and under the real RADV
//  driver on a host (tools/n48n-host).
//

#ifndef RDNA4N48N_hpp
#define RDNA4N48N_hpp

#include <stddef.h>
#include <stdint.h>

#include "../vulkan/navi48_native_abi.h"
#include "vmtree.hpp"

namespace N48N {

// IOReturn values, as the interface's contract names them.
constexpr uint32_t kSuccess = 0, kNoMemory = 0xe00002bd, kNoResources = 0xe00002be, kBadArgument = 0xe00002c2,
                   kUnsupported = 0xe00002c7, kBusy = 0xe00002d5, kTimeout = 0xe00002d6, kNotReady = 0xe00002d8,
                   kAborted = 0xe00002eb, kNotResponding = 0xe00002ed, kNotFound = 0xe00002f0;

// Per-client limits, the ones their kext and Mesa's stand-in use.
constexpr uint64_t kSystemCap = 512ull << 20, kSystemMaxBuffer = 64ull << 20;
constexpr uint32_t kMaxMaps = 4096;
constexpr uint64_t kVaFirst = 0x10000, kVaBits = 48;
// Work that makes no progress for this long is lost (amdgpu gives its graphics queue the same 10 s): the call
// that notices answers "timeout", every later submit and wait "aborted".
constexpr uint64_t kLostAfterNs = 10000000000ull;

// One buffer's storage, as the backend placed it.
struct Memory {
	uint64_t token;        // the backend's own name for it
	uint64_t physical;     // VRAM: GPU-physical address of its first byte (it is contiguous)
	bool     system;       // system memory: its pages are wherever Backend::systemPage says
	bool     cpuVisible;   // the CPU can map it
	bool     high;         // VRAM from the pool the CPU cannot reach
	uint64_t scanout;      // the address a display reads it by; 0 = it cannot be shown
};

struct Pools { uint64_t visibleTotal, visibleFree, highTotal, highFree; };

// One command buffer of a submission, at the client's own address.
struct Ib { uint64_t va; uint32_t dwords; };

struct Backend {
	void *context;
	// `bytes` are whole pages. VRAM the CPU can reach comes zeroed, and so does system memory.
	bool     (*allocVram)(void *context, uint64_t bytes, uint64_t align, bool highAllowed, Memory &out);
	bool     (*allocSystem)(void *context, uint64_t bytes, Memory &out);
	void     (*free)(void *context, const Memory &m, uint64_t bytes);
	uint64_t (*systemPage)(void *context, const Memory &m, uint64_t page);   // GPU-physical address of one page
	VmTree::Pages tables;
	void     (*flush)(void *context);        // the address space's translation caches, after its table changed
	bool     (*readRegs)(void *context, uint32_t dword, uint32_t count, uint32_t *out);
	void     (*pools)(void *context, Pools &out);

	// Work. The command buffers run in the client's address space, in order; when the card is through with them
	// it reports `sequence`. kSuccess; kBusy if the queue has no room until what is on it has finished; or why
	// the queue cannot take work at all.
	uint32_t (*submit)(void *context, const Ib *ibs, uint32_t count, uint32_t sequence);
	uint32_t (*finished)(void *context);     // the last sequence the card reported
	void     (*lost)(void *context);         // work did not finish: the queue is to fetch nothing more
	uint64_t (*now)(void *context);          // nanoseconds, monotonic
	void     (*pause)(void *context);        // a short sleep between two looks at `finished`
	// Eight bytes into a buffer the CPU reaches (the fence a submission asked for).
	void     (*store)(void *context, const Memory &m, uint64_t offset, uint64_t value);

	// One display for the client's pictures. scanQuery: what it shows now, as the interface has it (the
	// "acquired" fields are the caller's). scanAcquire: its plane is the client's from here on, kBusy if it is not
	// to be had. scanShow: the buffer at `address`, of this shape, shown from the next vertical blank, which is
	// waited for; the frame it appeared in. scanRelease: what the plane showed before is shown again (true if
	// the display confirms it).
	bool     (*scanQuery)(void *context, n48n_scan_query &q);
	uint32_t (*scanAcquire)(void *context);
	uint32_t (*scanShow)(void *context, uint64_t address, uint32_t width, uint32_t height, uint32_t pitchBytes,
	                     uint64_t &frame);
	bool     (*scanRelease)(void *context);
};

struct Buffer {
	Memory   memory;
	uint64_t bytes;        // 0 = the handle is free
	bool     uncached;
};

class Client {
public:
	// A fresh, empty address space. `vmid` and `build` are only reported back.
	bool open(const Backend &backend, uint32_t vmid, uint32_t build, uint32_t gcVersion);
	// Waits for the client's work on the queue (until it finished or counts as lost). False if it is lost.
	bool quiesce();
	// Everything the client still holds goes back: mappings, buffers, the table. Quiesces first.
	void close();

	// One IOConnectCallMethod: scalars in and out, a structure in and out. On entry *nOut and
	// *structOutSize are what the caller has room for, on return what was written. An IOReturn.
	uint32_t call(uint32_t selector, const uint64_t *in, uint32_t nIn, const void *structIn, size_t structInSize,
	              uint64_t *out, uint32_t *nOut, void *structOut, size_t *structOutSize);

	// For the CPU mapping of a buffer (IOConnectMapMemory64 with its handle); null if there is none.
	const Buffer *buffer(uint32_t handle) const;
	uint64_t root() const { return rootPage; }

private:
	struct Map { uint64_t va, bytes, offset; uint32_t handle, flags; };   // bytes 0 = free; va is the 48-bit one

	Backend  be {};
	bool     opened { false }, greeted { false };
	uint32_t vmid { 0 }, build { 0 }, gcVersion { 0 };
	uint64_t rootPage { 0 };
	uint64_t systemUsed { 0 };
	Buffer   buffers[N48N_MAX_BOS] {};
	Map      maps[kMaxMaps] {};
	bool     contexts[N48N_MAX_CTX + 1] {};
	// Work: sequences count from 1; `jobs` are the submissions the card has not reported yet, oldest first.
	struct Job { uint64_t sequence; uint32_t fenceHandle, fenceOffset; };   // fenceHandle 0 = no fence asked for
	uint64_t emitted { 0 }, retired { 0 }, progressAt { 0 };
	bool     lost { false };
	Job      jobs[N48N_FENCE_SLOTS] {};
	uint32_t jobCount { 0 };
	// The display: taken or not, the plane's shape when it was taken, and the buffers registered to be shown.
	struct Slot { uint64_t address, latchedFrame; uint32_t handle, presents; bool used; };
	bool     scanAcquired { false };
	uint32_t scanWidth { 0 }, scanHeight { 0 }, scanPitchBytes { 0 }, scanFront { N48N_SCAN_NO_SLOT };
	uint64_t scanConsole { 0 }, scanPresents { 0 }, scanRefused { 0 };
	Slot     slots[N48N_SCAN_MAX_SLOTS] {};

	uint32_t create(const n48n_gem_create_in &in, uint64_t *out);
	uint32_t release(uint32_t handle);
	uint32_t mapping(const n48n_gem_va &v);
	uint32_t context(const n48n_ctx &in, n48n_ctx &out);
	void     info(n48n_info &out);
	uint32_t submit(const void *structIn, size_t structInSize, uint64_t *out);
	bool     covered(uint64_t va, uint64_t bytes) const;
	void     retire();
	uint32_t waitFor(uint64_t sequence, uint64_t timeoutNs);
	uint32_t scan(uint32_t selector, const uint64_t *in, const void *structIn, uint64_t *out, void *structOut);
	bool     scanGiveBack();
	void     unmapTable(const Map &m);
	void     reset();
};

} // namespace N48N

#endif /* RDNA4N48N_hpp */
