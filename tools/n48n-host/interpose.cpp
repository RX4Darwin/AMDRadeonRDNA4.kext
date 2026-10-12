//
//  interpose.cpp
//  RDNA4FB
//
//  Puts this repository's client engine (src/n48n.cpp) under the real RADV
//  driver on a host, with no kext and no card: a library that, inserted into
//  a Vulkan program, answers the driver's IOKit calls (find the service,
//  open it, call a method, map a buffer) with the engine over ordinary
//  memory (hostbackend.hpp). What the driver asks for is then checked by the
//  same code the kext runs. Nothing executes, so submitting work is refused.
//
//  tools/n48n-host/run.sh builds it and runs vulkan/vkprobe.c and vulkan/n48nprobe.c on it.
//

#include <IOKit/IOKitLib.h>
#include <stdio.h>

#include "hostbackend.hpp"
#include "../../include/rdna4vulkan.h"
#include "../../src/gpuvm.hpp"

namespace {

constexpr io_service_t kService = 0x4e340001;
constexpr io_connect_t kConnection = 0x4e340002;

HostBackend host;
N48N::Client *client;        // allocated once: it is a few hundred KiB
bool open;
unsigned calls[32], refused;

bool tracing() {
	static const bool on = getenv("N48N_HOST_TRACE") != nullptr;
	return on;
}

CFMutableDictionaryRef matching(const char *) { return nullptr; }

io_service_t getService(mach_port_t, CFDictionaryRef) { return kService; }

kern_return_t serviceOpen(io_service_t service, task_port_t, uint32_t type, io_connect_t *connection) {
	if (service != kService || type != N48N_UC_TYPE)
		return kIOReturnBadArgument;
	if (open)
		return kIOReturnExclusiveAccess;   // one client at a time, as in the kext
	if (!client)
		client = new N48N::Client;
	if (!client->open(host.backend(), 8, 1, (12u << 16) | 1))
		return kIOReturnNoMemory;
	open = true;
	*connection = kConnection;
	return kIOReturnSuccess;
}

kern_return_t serviceClose(io_connect_t connection) {
	if (connection != kConnection || !open)
		return kIOReturnBadArgument;
	client->close();
	open = false;
	fprintf(stderr, "n48n-host: client closed: %u calls refused; left behind: %zu allocation(s), %llu table page(s), "
	        "%llu flush(es) so far\n", refused, host.memory.size(), (unsigned long long)host.tablePages,
	        (unsigned long long)host.flushes);
	return kIOReturnSuccess;
}

kern_return_t objectRelease(io_object_t) { return kIOReturnSuccess; }

// The bytes behind one page of the client's GPU address space, found through its page table as the card would;
// null if the page is not mapped with `need`.
uint8_t *pageAt(uint64_t va, uint64_t need) {
	const auto read = [](void *, uint64_t address, uint64_t &entry) {
		const auto table = host.memory.find(address & ~0xfffull);
		if (table == host.memory.end())
			return false;
		entry = static_cast<const uint64_t *>(table->second)[(address & 0xfff) / 8];
		return true;
	};
	uint64_t physical = 0, flags = 0;
	if (!GpuVm::walk(client->root(), va & ((1ull << N48N::kVaBits) - 1), read, nullptr, physical, flags) || !(flags & need))
		return nullptr;
	auto buffer = host.memory.upper_bound(physical);
	if (buffer == host.memory.begin())
		return nullptr;
	--buffer;
	return static_cast<uint8_t *>(buffer->second) + (physical - buffer->first);
}

// RDNA4_VULKAN_SEL_COPY_TEST with the kext's checks (src/n48nkext.cpp) and the CPU in place of the GPU's copy
// engine, so that vulkan/n48nprobe.c can be run before it is run on the card.
uint32_t copyTest(const uint64_t *in, uint32_t nIn) {
	if (nIn != 3 || !in[2] || in[2] > RDNA4_VULKAN_COPY_TEST_MAX || ((in[0] | in[1] | in[2]) & 0xfff))
		return N48N::kBadArgument;
	for (uint64_t at = 0; at < in[2]; at += 4096)
		if (!pageAt(in[0] + at, GpuVm::kReadable) || !pageAt(in[1] + at, GpuVm::kWritable))
			return N48N::kBadArgument;
	for (uint64_t at = 0; at < in[2]; at += 4096)
		memcpy(pageAt(in[1] + at, GpuVm::kWritable), pageAt(in[0] + at, GpuVm::kReadable), 4096);
	return N48N::kSuccess;
}

kern_return_t callMethod(mach_port_t connection, uint32_t selector, const uint64_t *in, uint32_t nIn, const void *structIn,
                         size_t structInSize, uint64_t *out, uint32_t *nOut, void *structOut, size_t *structOutSize) {
	if (connection != kConnection || !open)
		return kIOReturnBadArgument;
	const uint32_t r = selector == RDNA4_VULKAN_SEL_COPY_TEST
		? copyTest(in, nIn)
		: client->call(selector, in, nIn, structIn, structInSize, out, nOut, structOut, structOutSize);
	if (selector < 32)
		calls[selector]++;
	refused += r != N48N::kSuccess;
	if (tracing() || r != N48N::kSuccess)
		fprintf(stderr, "n48n-host: selector %u -> 0x%08x\n", selector, r);
	return static_cast<kern_return_t>(r);
}

kern_return_t mapMemory(io_connect_t connection, uint32_t handle, task_port_t, mach_vm_address_t *address,
                        mach_vm_size_t *size, IOOptionBits) {
	const N48N::Buffer *b = connection == kConnection && open ? client->buffer(handle) : nullptr;
	if (!b)
		return kIOReturnNotFound;
	if (!b->memory.cpuVisible)
		return kIOReturnNotPermitted;
	*address = reinterpret_cast<mach_vm_address_t>(host.bytesOf(b->memory));
	*size = b->bytes;
	return kIOReturnSuccess;
}

kern_return_t unmapMemory(io_connect_t, uint32_t, task_port_t, mach_vm_address_t) { return kIOReturnSuccess; }

#define INTERPOSE(ours, theirs)                                                                              \
	__attribute__((used)) const struct { const void *replacement, *original; } interpose_##theirs            \
		__attribute__((section("__DATA,__interpose"))) = { reinterpret_cast<const void *>(&ours),            \
		                                                   reinterpret_cast<const void *>(&theirs) }
INTERPOSE(matching, IOServiceMatching);
INTERPOSE(getService, IOServiceGetMatchingService);
INTERPOSE(serviceOpen, IOServiceOpen);
INTERPOSE(serviceClose, IOServiceClose);
INTERPOSE(objectRelease, IOObjectRelease);
INTERPOSE(callMethod, IOConnectCallMethod);
INTERPOSE(mapMemory, IOConnectMapMemory64);
INTERPOSE(unmapMemory, IOConnectUnmapMemory64);

} // namespace
