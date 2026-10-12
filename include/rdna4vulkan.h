/*
 *  rdna4vulkan.h
 *  RDNA4FB
 *
 *  What this kext adds to the interface the RADV Darwin port speaks
 *  (vulkan/navi48_native_abi.h; docs/vulkan-port.md): where the interface is
 *  found, and one call of its own for testing. Plain C, shared by the kext
 *  (src/n48nkext.cpp) and user space (vulkan/n48nprobe.c).
 *
 *  The interface is a user client of type N48N_UC_TYPE on the service the
 *  compute bring-up publishes (rdna4-compute=6 or 7). Root only.
 */

#ifndef RDNA4Vulkan_h
#define RDNA4Vulkan_h

#define RDNA4_VULKAN_SERVICE "RDNA4ComputeService"

/* in: [0] source GPU address [1] destination GPU address [2] bytes (whole
 * pages, at most RDNA4_VULKAN_COPY_TEST_MAX). The GPU's copy engine, told to
 * work in the caller's address space, copies between two ranges the caller
 * mapped with GemVa; the kext first checks in the caller's page table that
 * both are mapped, the source readable and the destination writable. It shows
 * that the address space works before anything is drawn in it. Not a part of
 * the Vulkan interface: RADV never calls it. */
#define RDNA4_VULKAN_SEL_COPY_TEST 0x52440001u
#define RDNA4_VULKAN_COPY_TEST_MAX (1u << 20)

#endif /* RDNA4Vulkan_h */
