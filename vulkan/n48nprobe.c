/*
 *  n48nprobe.c
 *  RDNA4FB
 *
 *  The card test of the Vulkan interface's memory half (docs/vulkan-port.md,
 *  step 1), without Mesa: open the interface, make a buffer of each kind
 *  (VRAM the CPU reaches, VRAM it does not, system memory), map them at three
 *  far-apart addresses of the process's GPU address space, and have the GPU's
 *  copy engine carry a pattern round through all three in that address space
 *  (include/rdna4vulkan.h). The pattern coming back right shows the page
 *  table, the address space's registers and every kind of memory at once.
 *
 *    make n48nprobe && sudo build/n48nprobe
 *
 *  Needs the kext's compute bring-up (rdna4-compute=7) and root. A copy the
 *  GPU cannot finish stops its copy engine until the next boot; the display
 *  is not involved. Exit status 0 = every line says ok.
 */

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "navi48_native_abi.h"
#include "rdna4vulkan.h"

#define BYTES (256u << 10)

static io_connect_t conn;
static int failures;

static void check(int ok, const char *what) {
	printf("%-62s %s\n", what, ok ? "ok" : "FAILED");
	failures += !ok;
}

static kern_return_t call(uint32_t selector, const uint64_t *in, uint32_t nIn, const void *structIn, size_t structInSize,
                          uint64_t *out, uint32_t nOut, void *structOut, size_t structOutSize) {
	return IOConnectCallMethod(conn, selector, in, nIn, structIn, structInSize, out, nOut ? &nOut : NULL, structOut,
	                           structOutSize ? &structOutSize : NULL);
}

static kern_return_t openInterface(void) {
	io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching(RDNA4_VULKAN_SERVICE));
	if (!service)
		return kIOReturnNotFound;
	const kern_return_t r = IOServiceOpen(service, mach_task_self(), N48N_UC_TYPE, &conn);
	IOObjectRelease(service);
	return r;
}

static int info(struct n48n_info *i) { return call(N48N_SEL_QUERYINFO, NULL, 0, NULL, 0, NULL, 0, i, sizeof(*i)) == 0; }

/* A buffer: its handle, or 0. `placed` gets the placement bits. */
static uint32_t create(uint64_t domain, uint64_t flags, uint64_t *placed) {
	const struct n48n_gem_create_in in = { BYTES, 4096, domain, flags };
	uint64_t out[4] = { 0 };
	if (call(N48N_SEL_BOCREATE, NULL, 0, &in, sizeof(in), out, 4, NULL, 0) != 0)
		return 0;
	*placed = out[3];
	return (uint32_t)out[0];
}

static int mapAt(uint32_t handle, uint64_t va, uint32_t operation) {
	struct n48n_gem_va v;
	memset(&v, 0, sizeof(v));
	v.handle = handle;
	v.operation = operation;
	v.flags = N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE | N48N_VM_PAGE_EXECUTABLE;   /* what RADV maps everything with */
	v.va_address = va;
	v.map_size = BYTES;
	return call(N48N_SEL_GEMVA, NULL, 0, &v, sizeof(v), NULL, 0, NULL, 0) == 0;
}

static uint32_t *cpuMap(uint32_t handle) {
	mach_vm_address_t address = 0;
	mach_vm_size_t size = 0;
	return IOConnectMapMemory64(conn, handle, mach_task_self(), &address, &size, kIOMapAnywhere) == 0 && size >= BYTES
		? (uint32_t *)(uintptr_t)address : NULL;
}

static kern_return_t copy(uint64_t from, uint64_t to) {
	const uint64_t in[3] = { from, to, BYTES };
	return call(RDNA4_VULKAN_SEL_COPY_TEST, in, 3, NULL, 0, NULL, 0, NULL, 0);
}

static uint32_t pattern(uint32_t i) { return (i * 2654435761u) ^ 0x52444e41u; }

static int holdsPattern(const uint32_t *p) {
	for (uint32_t i = 0; i < BYTES / 4; i++)
		if (p[i] != pattern(i)) {
			printf("  dword %u: 0x%08x, expected 0x%08x\n", i, p[i], pattern(i));
			return 0;
		}
	return 1;
}

int main(void) {
	/* Far apart, so that every level of the table is used: 4 GiB, the top of the lower half, the upper half. */
	const uint64_t vaVisible = 0x0000000100000000ull, vaHigh = 0x00007ffe00000000ull, vaSystem = 0xffff800000200000ull;

	kern_return_t r = openInterface();
	if (r != 0) {
		printf("open: 0x%08x%s\n", r,
		       r == (kern_return_t)kIOReturnNotFound ? " (no " RDNA4_VULKAN_SERVICE ": did the bring-up finish? rdna4-compute=7)"
		       : r == (kern_return_t)kIOReturnNotPrivileged ? " (needs root)"
		       : r == (kern_return_t)kIOReturnNotReady ? " (the runtime is not ready, has no DMA, or rdna4-vm is set)" : "");
		return 1;
	}
	const uint64_t hello[2] = { N48N_ABI_VERSION, 0 };
	uint64_t greeting[4] = { 0 };
	check(call(N48N_SEL_HELLO, hello, 2, NULL, 0, greeting, 4, NULL, 0) == 0, "hello");
	struct n48n_info before, after;
	check(info(&before), "query info");
	printf("  address space %u, GC %u.%u.%u, GB_ADDR_CONFIG 0x%08x\n  VRAM behind the BAR %llu MiB (%llu free), past it "
	       "%llu MiB (%llu free)\n", before.vmid, before.gc_version >> 16, (before.gc_version >> 8) & 0xff,
	       before.gc_version & 0xff, before.gb_addr_config, before.vram_vis_total >> 20, before.vram_vis_free >> 20,
	       before.vram_hi_total >> 20, before.vram_hi_free >> 20);

	uint64_t placedVisible = 0, placedHigh = 0, placedSystem = 0;
	const uint32_t visible = create(N48N_GEM_DOMAIN_VRAM, N48N_GEM_CPU_ACCESS_REQUIRED, &placedVisible);
	const uint32_t high = create(N48N_GEM_DOMAIN_VRAM, N48N_GEM_NO_CPU_ACCESS, &placedHigh);
	const uint32_t system = create(N48N_GEM_DOMAIN_GTT, 0, &placedSystem);
	check(visible && (placedVisible & N48N_PLACED_CPU_MAPPABLE), "a buffer in VRAM the CPU reaches");
	check(high != 0, "a buffer in VRAM the CPU need not reach");
	printf("  it is %s\n", placedHigh & N48N_PLACED_HI_POOL ? "past the BAR" : "behind the BAR (nothing past it to give)");
	check(system != 0, "a buffer in system memory");
	check(mapAt(visible, vaVisible, N48N_VA_OP_MAP) && mapAt(high, vaHigh, N48N_VA_OP_MAP) &&
	      mapAt(system, vaSystem, N48N_VA_OP_MAP), "all three mapped in the GPU address space");
	uint32_t *cpuVisible = cpuMap(visible), *cpuSystem = cpuMap(system);
	check(cpuVisible && cpuSystem, "the first and the third mapped into this process");
	if (failures) {
		printf("stopping before the GPU is asked to copy\n");
		return 1;
	}

	for (uint32_t i = 0; i < BYTES / 4; i++)
		cpuVisible[i] = pattern(i);
	check(holdsPattern(cpuVisible), "the CPU reads back what it wrote to VRAM");
	check(copy(vaVisible + BYTES, vaHigh) == (kern_return_t)kIOReturnBadArgument, "a copy from an address not mapped is refused");
	/* Each copy is only tried if the one before it finished: a copy that hangs stops the engine. */
	r = copy(vaVisible, vaHigh);
	check(r == 0, "GPU copy: VRAM behind the BAR -> the second buffer");
	if (r == 0) {
		r = copy(vaHigh, vaSystem);
		check(r == 0, "GPU copy: the second buffer -> system memory");
	}
	if (r == 0) {
		check(holdsPattern(cpuSystem), "system memory holds the pattern");
		memset(cpuVisible, 0, BYTES);
		r = copy(vaSystem, vaVisible);
		check(r == 0, "GPU copy: system memory -> VRAM behind the BAR");
	}
	if (r == 0)
		check(holdsPattern(cpuVisible), "VRAM behind the BAR holds the pattern again");
	else
		printf("  the copy answered 0x%08x; the kernel log has the copy engine's state (\"vulkan:\" and \"dma:\" lines)\n", r);

	check(mapAt(visible, vaVisible, N48N_VA_OP_UNMAP) && mapAt(high, vaHigh, N48N_VA_OP_UNMAP) &&
	      mapAt(system, vaSystem, N48N_VA_OP_UNMAP), "all three unmapped");
	const uint64_t handles[3] = { visible, high, system };
	int freed = 1;
	for (int i = 0; i < 3; i++)
		freed &= call(N48N_SEL_BOFREE, &handles[i], 1, NULL, 0, NULL, 0, NULL, 0) == 0;
	check(freed, "all three freed");
	check(info(&after) && after.vram_hi_free == before.vram_hi_free && after.gtt_used == before.gtt_used,
	      "VRAM past the BAR and system memory are back as they were");
	IOServiceClose(conn);
	r = openInterface();
	check(r == 0, "the interface opens again after the close");
	if (r == 0)
		IOServiceClose(conn);

	printf("%s\n", failures ? "FAILED" : "all ok");
	return failures != 0;
}
