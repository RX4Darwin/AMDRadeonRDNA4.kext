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
//  tools/n48n-host/run.sh builds it and runs vulkan/vkprobe.c on it.
//

#include <IOKit/IOKitLib.h>
#include <stdio.h>

#include "hostbackend.hpp"

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

kern_return_t callMethod(mach_port_t connection, uint32_t selector, const uint64_t *in, uint32_t nIn, const void *structIn,
                         size_t structInSize, uint64_t *out, uint32_t *nOut, void *structOut, size_t *structOutSize) {
	if (connection != kConnection || !open)
		return kIOReturnBadArgument;
	const uint32_t r = client->call(selector, in, nIn, structIn, structInSize, out, nOut, structOut, structOutSize);
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
