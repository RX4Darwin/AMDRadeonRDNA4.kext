//
//  n48n.hpp
//  RDNA4FB
//
//  The kernel half of the interface the RADV Darwin port speaks
//  (vulkan/navi48_native_abi.h, "N48N"; docs/vulkan-port.md): one client's
//  buffers, its GPU address space and its contexts, behind the calls
//  IOConnectCallMethod carries. This is the memory half (selectors 0 to 6);
//  submitting work and waiting for it (7, 8) come with the graphics queue.
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
                   kUnsupported = 0xe00002c7, kNotReady = 0xe00002d8, kNotFound = 0xe00002f0;

// Per-client limits, the ones their kext and Mesa's stand-in use.
constexpr uint64_t kSystemCap = 512ull << 20, kSystemMaxBuffer = 64ull << 20;
constexpr uint32_t kMaxMaps = 4096;
constexpr uint64_t kVaFirst = 0x10000, kVaBits = 48;

// One buffer's storage, as the backend placed it.
struct Memory {
	uint64_t token;        // the backend's own name for it
	uint64_t physical;     // VRAM: GPU-physical address of its first byte (it is contiguous)
	bool     system;       // system memory: its pages are wherever Backend::systemPage says
	bool     cpuVisible;   // the CPU can map it
	bool     high;         // VRAM from the pool the CPU cannot reach
};

struct Pools { uint64_t visibleTotal, visibleFree, highTotal, highFree; };

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
	// Everything the client still holds goes back: mappings, buffers, the table.
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

	uint32_t create(const n48n_gem_create_in &in, uint64_t *out);
	uint32_t release(uint32_t handle);
	uint32_t mapping(const n48n_gem_va &v);
	uint32_t context(const n48n_ctx &in, n48n_ctx &out);
	void     info(n48n_info &out) const;
	void     unmapTable(const Map &m);
	void     reset();
};

} // namespace N48N

#endif /* RDNA4N48N_hpp */
