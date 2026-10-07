/*
 *  librdna4.c
 *  RDNA4FB
 *
 *  See librdna4.h.
 */

#include "librdna4.h"

#include <mach/mach.h>
#include <mach/mach_error.h>
#include <string.h>

kern_return_t rdna4_open(rdna4_t *dev) {
	dev->conn = IO_OBJECT_NULL;
	/* MACH_PORT_NULL: the default main port, on every macOS version. */
	io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL,
	                                               IOServiceMatching(RDNA4_COMPUTE_SERVICE));
	if (svc == IO_OBJECT_NULL)
		return kIOReturnNotFound;
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &dev->conn);
	IOObjectRelease(svc);
	return kr;
}

void rdna4_close(rdna4_t *dev) {
	if (dev->conn != IO_OBJECT_NULL)
		IOServiceClose(dev->conn);
	dev->conn = IO_OBJECT_NULL;
}

kern_return_t rdna4_info(rdna4_t *dev, rdna4_info_t *out) {
	uint64_t o[9] = { 0 };
	uint32_t n = 9;
	kern_return_t kr = IOConnectCallScalarMethod(dev->conn, kRDNA4MethodInfo, NULL, 0, o, &n);
	if (kr == KERN_SUCCESS) {
		out->abi = o[0];
		out->stage = o[1];
		out->flags = o[2];
		out->heapBytes = o[3];
		out->heapFree = o[4];
		out->heapBase = o[5];
		out->vmid = o[6];
		out->pipe = o[7];
		out->queue = o[8];
	}
	return kr;
}

kern_return_t rdna4_sensors(rdna4_t *dev, RDNA4Sensors *out) {
	if (!out)
		return kIOReturnBadArgument;
	size_t n = sizeof(*out);
	return IOConnectCallStructMethod(dev->conn, kRDNA4MethodSensors, NULL, 0,
	                                 out, &n);
}

kern_return_t rdna4_sensors_ex(rdna4_t *dev, RDNA4SensorsEx *out) {
	if (!out)
		return kIOReturnBadArgument;
	size_t n = sizeof(*out);
	return IOConnectCallStructMethod(dev->conn, kRDNA4MethodSensorsEx, NULL, 0,
	                                 out, &n);
}

kern_return_t rdna4_sleep_test(rdna4_t *dev, uint32_t phase) {
	uint64_t input = phase;
	return IOConnectCallScalarMethod(dev->conn, kRDNA4MethodSleepTest, &input, 1, NULL, NULL);
}

kern_return_t rdna4_quiesce(rdna4_t *dev) {
	return IOConnectCallScalarMethod(dev->conn, kRDNA4MethodQuiesce, NULL, 0, NULL, NULL);
}

kern_return_t rdna4_alloc(rdna4_t *dev, uint64_t bytes, rdna4_buffer_t *out) {
	uint64_t o[2] = { 0 };
	uint32_t n = 2;
	kern_return_t kr = IOConnectCallScalarMethod(dev->conn, kRDNA4MethodAlloc, &bytes, 1, o, &n);
	if (kr == KERN_SUCCESS) {
		out->handle = o[0];
		out->gpu = o[1];
		out->bytes = bytes;
	}
	return kr;
}

kern_return_t rdna4_alloc_host(rdna4_t *dev, uint64_t bytes, rdna4_buffer_t *out,
                                void **cpu) {
	const uint64_t in[2] = { bytes, 0 };
	uint64_t o[3] = { 0 };
	uint32_t n = 3;
	kern_return_t kr = IOConnectCallScalarMethod(dev->conn, kRDNA4MethodAllocHost,
	                                             in, 2, o, &n);
	if (kr == KERN_SUCCESS) {
		out->handle = o[0];
		out->gpu = o[1];
		out->bytes = bytes;
		if (cpu)
			*cpu = (void *)(uintptr_t)o[2];
	}
	return kr;
}

kern_return_t rdna4_free(rdna4_t *dev, const rdna4_buffer_t *buf) {
	return IOConnectCallScalarMethod(dev->conn, kRDNA4MethodFree, &buf->handle, 1, NULL, NULL);
}

static kern_return_t copy(rdna4_t *dev, uint32_t sel, const rdna4_buffer_t *buf, uint64_t offset,
                          const void *host, uint64_t bytes) {
	const uint64_t in[4] = { buf->handle, offset, (uint64_t)(uintptr_t)host, bytes };
	return IOConnectCallScalarMethod(dev->conn, sel, in, 4, NULL, NULL);
}

kern_return_t rdna4_write(rdna4_t *dev, const rdna4_buffer_t *buf, uint64_t offset,
                          const void *src, uint64_t bytes) {
	return copy(dev, kRDNA4MethodWrite, buf, offset, src, bytes);
}

kern_return_t rdna4_read(rdna4_t *dev, const rdna4_buffer_t *buf, uint64_t offset,
                         void *dst, uint64_t bytes) {
	return copy(dev, kRDNA4MethodRead, buf, offset, dst, bytes);
}

kern_return_t rdna4_load(rdna4_t *dev, const void *elf, size_t bytes, const char *kernel,
                         rdna4_program_t *out) {
	const size_t nameBytes = strlen(kernel) + 1;
	if (nameBytes > RDNA4_MAX_NAME)
		return kIOReturnBadArgument;
	const uint64_t in[2] = { (uint64_t)(uintptr_t)elf, bytes };
	uint64_t o[9] = { 0 };
	uint32_t n = 9;
	kern_return_t kr = IOConnectCallMethod(dev->conn, kRDNA4MethodLoad, in, 2, kernel, nameBytes,
	                                       o, &n, NULL, NULL);
	if (kr == KERN_SUCCESS) {
		out->handle = o[0];
		out->gpu = o[8];
		out->kernargBytes = o[1];
		out->imageBytes = o[2];
		out->rsrc1 = o[3];
		out->rsrc2 = o[4];
		out->rsrc3 = o[5];
		out->properties = o[6];
		out->ldsBytes = o[7];
	}
	return kr;
}

kern_return_t rdna4_submit_ib(rdna4_t *dev, const rdna4_buffer_t *buf, uint64_t offsetBytes,
                              uint32_t dwords, uint64_t *fence) {
	if (!buf || offsetBytes > buf->bytes || offsetBytes > UINT64_MAX - buf->gpu)
		return kIOReturnBadArgument;
	const uint64_t in[3] = { buf->gpu + offsetBytes, dwords, 0 };
	uint64_t out = 0;
	uint32_t n = 1;
	kern_return_t kr = IOConnectCallScalarMethod(dev->conn, kRDNA4MethodSubmitIb, in, 3,
	                                             &out, &n);
	if (kr == KERN_SUCCESS && fence)
		*fence = out;
	return kr;
}

kern_return_t rdna4_wait_fence(rdna4_t *dev, uint64_t fence, uint32_t timeoutMs,
                               uint64_t *ns) {
	if (fence > UINT32_MAX)
		return kIOReturnBadArgument;
	const uint64_t in[2] = { fence, timeoutMs };
	uint64_t out = 0;
	uint32_t n = 1;
	kern_return_t kr = IOConnectCallScalarMethod(dev->conn, kRDNA4MethodWaitFence, in, 2,
	                                             &out, &n);
	if (ns)
		*ns = out;
	return kr;
}

kern_return_t rdna4_submit_gfx_ib(rdna4_t *dev, const rdna4_buffer_t *buf, uint64_t offsetBytes,
                                  uint32_t dwords, uint64_t *fence) {
	if (!buf || offsetBytes > buf->bytes || offsetBytes > UINT64_MAX - buf->gpu)
		return kIOReturnBadArgument;
	const uint64_t in[3] = { buf->gpu + offsetBytes, dwords, 0 };
	uint64_t out = 0;
	uint32_t n = 1;
	kern_return_t kr = IOConnectCallScalarMethod(dev->conn, kRDNA4MethodSubmitGfxIb, in, 3, &out, &n);
	if (kr == KERN_SUCCESS && fence)
		*fence = out;
	return kr;
}

kern_return_t rdna4_wait_gfx_fence(rdna4_t *dev, uint64_t fence, uint32_t timeoutMs, uint64_t *ns) {
	if (fence > UINT32_MAX)
		return kIOReturnBadArgument;
	const uint64_t in[2] = { fence, timeoutMs };
	uint64_t out = 0;
	uint32_t n = 1;
	kern_return_t kr = IOConnectCallScalarMethod(dev->conn, kRDNA4MethodWaitGfxFence, in, 2, &out, &n);
	if (ns)
		*ns = out;
	return kr;
}

kern_return_t rdna4_unload(rdna4_t *dev, const rdna4_program_t *prog) {
	return IOConnectCallScalarMethod(dev->conn, kRDNA4MethodUnload, &prog->handle, 1, NULL, NULL);
}

kern_return_t rdna4_dispatch(rdna4_t *dev, const rdna4_program_t *prog, const uint32_t groups[3],
                             const uint32_t groupSize[3], const void *kernargs,
                             uint32_t kernargBytes, uint32_t timeoutMs, uint64_t *micros) {
	return rdna4_dispatch_lds(dev, prog, groups, groupSize, kernargs, kernargBytes, 0, timeoutMs,
	                          micros);
}

kern_return_t rdna4_dispatch_lds(rdna4_t *dev, const rdna4_program_t *prog,
                                 const uint32_t groups[3], const uint32_t groupSize[3],
                                 const void *kernargs, uint32_t kernargBytes,
                                 uint32_t dynamicLdsBytes, uint32_t timeoutMs, uint64_t *micros) {
	if (kernargBytes > RDNA4_MAX_KERNARG || prog->handle > 0xffffffffu)
		return kIOReturnBadArgument;
	RDNA4Dispatch d;
	memset(&d, 0, sizeof(d));
	d.program = (uint32_t)prog->handle;
	for (int i = 0; i < 3; i++) {
		d.groups[i] = groups[i];
		d.groupSize[i] = groupSize[i];
	}
	d.timeoutMs = timeoutMs;
	d.kernargBytes = kernargBytes;
	d.dynamicLdsBytes = dynamicLdsBytes;
	if (kernargBytes)
		memcpy(d.kernargs, kernargs, kernargBytes);
	uint64_t us = 0;
	uint32_t n = 1;
	kern_return_t kr = IOConnectCallMethod(dev->conn, kRDNA4MethodDispatch, NULL, 0, &d, sizeof(d),
	                                       &us, &n, NULL, NULL);
	if (micros)
		*micros = us;
	return kr;
}

kern_return_t rdna4_wait_vblank(rdna4_t *dev, uint32_t timeoutMs, uint64_t *count,
                                uint64_t *timeNs) {
	uint64_t in = timeoutMs, out[2] = { 0, 0 };
	uint32_t n = 2;
	kern_return_t kr = IOConnectCallScalarMethod(dev->conn, kRDNA4MethodWaitVBlank, &in, 1,
	                                             out, &n);
	if (kr == KERN_SUCCESS) {
		if (count)
			*count = out[0];
		if (timeNs)
			*timeNs = out[1];
	}
	return kr;
}

kern_return_t rdna4_present(rdna4_t *dev, const rdna4_buffer_t *buf, uint64_t offset,
                            uint32_t *width, uint32_t *height, uint32_t *pitch) {
	if (!buf || !width || !height || !pitch)
		return kIOReturnBadArgument;
	const uint64_t in[2] = { buf->handle, offset };
	uint64_t out[2] = { 0, 0 };
	uint32_t n = 2;
	kern_return_t kr = IOConnectCallScalarMethod(dev->conn, kRDNA4MethodPresent, in, 2, out, &n);
	if (kr == KERN_SUCCESS) {
		*width = (uint32_t)out[0] & 0xffffu;
		*height = (uint32_t)(out[0] >> 16);
		*pitch = (uint32_t)out[1];
	}
	return kr;
}

kern_return_t rdna4_present_async(rdna4_t *dev, const rdna4_buffer_t *buf, uint64_t offset,
                                   uint64_t *presentId) {
	if (!buf || !presentId)
		return kIOReturnBadArgument;
	const uint64_t in[2] = { buf->handle, offset };
	uint64_t out = 0;
	uint32_t n = 1;
	kern_return_t kr = IOConnectCallScalarMethod(dev->conn, kRDNA4MethodPresentAsync, in, 2,
	                                              &out, &n);
	if (kr == KERN_SUCCESS)
		*presentId = out;
	return kr;
}

kern_return_t rdna4_wait_present(rdna4_t *dev, uint64_t presentId, uint32_t timeoutMs,
                                  uint64_t *frame) {
	if (!presentId || !frame)
		return kIOReturnBadArgument;
	const uint64_t in[2] = { presentId, timeoutMs };
	uint64_t out = 0;
	uint32_t n = 1;
	kern_return_t kr = IOConnectCallScalarMethod(dev->conn, kRDNA4MethodWaitPresent, in, 2,
	                                              &out, &n);
	if (kr == KERN_SUCCESS)
		*frame = out;
	return kr;
}

kern_return_t rdna4_display_query(rdna4_t *dev, uint32_t *width, uint32_t *height,
                                  uint32_t *pitch) {
	rdna4_buffer_t query = { 0, 0, 0 };
	return rdna4_present(dev, &query, 0, width, height, pitch);
}

kern_return_t rdna4_restore(rdna4_t *dev) {
	return IOConnectCallScalarMethod(dev->conn, kRDNA4MethodRestore, NULL, 0, NULL, NULL);
}

const char *rdna4_error(kern_return_t kr) {
	return mach_error_string(kr);
}
