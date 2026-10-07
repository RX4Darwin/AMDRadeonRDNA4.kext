//
//  vkprobe.c
//  RDNA4FB
//
//  Drives the RADV Darwin build directly through its ICD entry point (no Vulkan loader needed): instance, the
//  one device, a buffer with memory, two fills of it by the GPU and a triangle drawn into an image and copied back,
//  each submitted, waited for and checked. With "show [seconds]" after the library's path it then puts a moving
//  triangle on the boot display for that long (5 s) and gives the desktop back. With "fault" it instead fills
//  and copies through memory that is not there, and says what became of that work.
//
//    make mesa        (the driver and this program, into build/), or by hand:
//    clang -arch x86_64 -mmacosx-version-min=11.0 -std=gnu11 -I <work>/mesa/include vulkan/vkprobe.c -o vkprobe
//    (the two shaders are in shaders/: vkprobe_vert.h and vkprobe_frag.h, made from vkprobe.vert and vkprobe.frag
//    there with glslangValidator -V --vn vkprobe_vert -o vkprobe_vert.h vkprobe.vert, and the same for frag)
//    RADV_DARWIN_MOCK=1 VKPROBE_NOGPU=1 ./vkprobe <work>/build/src/amd/vulkan/libvulkan_radeon.dylib
//
//  RADV_DARWIN_MOCK=1 puts Mesa's in-process stand-in in place of the kext; RADV_DARWIN_TRACE=1 logs every call
//  the driver makes to the kext interface. Against a real kext, leave RADV_DARWIN_MOCK out and run as root.
//  With a stand-in nothing executes: VKPROBE_NOGPU=1 then keeps the fills and the picture from counting as failures.
//
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "navi48_native_abi.h"
#include "shaders/vkprobe_frag.h"   /* shaders/vkprobe.frag and .vert through glslangValidator -V --vn */
#include "shaders/vkprobe_vert.h"

typedef PFN_vkVoidFunction (*GetProc)(VkInstance, const char *);
#define F(inst, name) PFN_##name name = (PFN_##name)gip(inst, #name); if (!name) { printf("no %s\n", #name); return 1; }
#define OK(call) do { VkResult r_ = (call); printf("%-34s -> %d\n", #call, r_); if (r_ < 0) return 1; } while (0)

int main(int argc, char **argv) {
	void *lib = dlopen(argc > 1 ? argv[1] : "libvulkan_radeon.dylib", RTLD_NOW);
	if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
	GetProc gip = (GetProc)dlsym(lib, "vk_icdGetInstanceProcAddr");
	if (!gip) { printf("no vk_icdGetInstanceProcAddr\n"); return 1; }
	F(NULL, vkCreateInstance)
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vkprobe", 1, "none", 0, VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
	VkInstance inst;
	OK(vkCreateInstance(&ici, 0, &inst));
	F(inst, vkEnumeratePhysicalDevices) F(inst, vkGetPhysicalDeviceProperties) F(inst, vkGetPhysicalDeviceMemoryProperties)
	F(inst, vkGetPhysicalDeviceQueueFamilyProperties) F(inst, vkCreateDevice) F(inst, vkGetDeviceProcAddr)
	uint32_t n = 4; VkPhysicalDevice pd[4];
	OK(vkEnumeratePhysicalDevices(inst, &n, pd));
	printf("physical devices: %u\n", n);
	if (!n) return 2;
	VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd[0], &pp);
	printf("device: %s, API %u.%u.%u, vendor 0x%04x device 0x%04x\n", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion),
	       VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion), pp.vendorID, pp.deviceID);
	VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd[0], &mp);
	for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
		printf("heap %u: %llu MiB flags 0x%x\n", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20), mp.memoryHeaps[i].flags);
	uint32_t nq = 8; VkQueueFamilyProperties qf[8]; vkGetPhysicalDeviceQueueFamilyProperties(pd[0], &nq, qf);
	uint32_t fam = 0; for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { fam = i; break; }
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, fam, 1, &prio };
	VkPhysicalDeviceVulkan13Features v13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &v13, 0, 1, &qci };
	VkDevice dev;
	OK(vkCreateDevice(pd[0], &dci, 0, &dev));
#define D(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name); if (!name) { printf("no %s\n", #name); return 1; }
	D(vkGetDeviceQueue) D(vkCreateBuffer) D(vkGetBufferMemoryRequirements) D(vkAllocateMemory) D(vkBindBufferMemory) D(vkMapMemory)
	D(vkCreateCommandPool) D(vkAllocateCommandBuffers) D(vkBeginCommandBuffer) D(vkCmdFillBuffer) D(vkEndCommandBuffer)
	D(vkCreateFence) D(vkQueueSubmit) D(vkWaitForFences) D(vkResetFences) D(vkDeviceWaitIdle) D(vkDestroyFence) D(vkDestroyCommandPool)
	D(vkDestroyBuffer) D(vkFreeMemory) D(vkDestroyDevice)
	D(vkCreateImage) D(vkGetImageMemoryRequirements) D(vkBindImageMemory) D(vkCreateImageView) D(vkCreateShaderModule)
	D(vkCreatePipelineLayout) D(vkCreateGraphicsPipelines) D(vkCmdPipelineBarrier) D(vkCmdBeginRendering) D(vkCmdEndRendering)
	D(vkCmdBindPipeline) D(vkCmdSetViewport) D(vkCmdSetScissor) D(vkCmdDraw) D(vkCmdCopyImageToBuffer) D(vkDestroyPipeline)
	D(vkDestroyPipelineLayout) D(vkDestroyShaderModule) D(vkDestroyImageView) D(vkDestroyImage)
	VkQueue q; vkGetDeviceQueue(dev, fam, 0, &q);
	enum { kBytes = 1 << 18 };
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer buf; OK(vkCreateBuffer(dev, &bci, 0, &buf));
	VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
	uint32_t type = 0;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { type = i; break; }
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, type };
	printf("buffer memory: type %u (flags 0x%x) of heap %u (flags 0x%x)\n", type, mp.memoryTypes[type].propertyFlags,
	       mp.memoryTypes[type].heapIndex, mp.memoryHeaps[mp.memoryTypes[type].heapIndex].flags);
	VkDeviceMemory mem; OK(vkAllocateMemory(dev, &mai, 0, &mem));
	OK(vkBindBufferMemory(dev, buf, mem, 0));
	void *p = 0; OK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &p));
	memset(p, 0x11, kBytes);
	VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, fam };
	VkCommandPool pool; OK(vkCreateCommandPool(dev, &cpi, 0, &pool));
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 3 };
	VkCommandBuffer cb[3]; OK(vkAllocateCommandBuffers(dev, &cai, cb));
	VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence; OK(vkCreateFence(dev, &fci, 0, &fence));
	/* Two kinds of GPU work, the simpler first: RADV fills under 4 KiB with the command processor's own copy (no
	 * shader), and larger ones with a compute shader; but in system memory, which this buffer is in, only above
	 * 64 KiB (radv_prefer_compute_or_cp_dma, RADV_BUFFER_OPS_GTT_CP_DMA_MAX_BYTES). Until 2026-10-07 the second
	 * fill here was 61440 bytes, so it too was the command processor's, whatever this program called it. */
	const struct { VkDeviceSize offset, size; uint32_t value; const char *by; } fills[2] = {
		{ 0, 1024, 0xcafef00d, "the command processor" }, { 4096, kBytes - 4096, 0x0badf00d, "a compute shader" } };
	int wrong = 0;
	for (int f = 0; f < 2; f++) {
		VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		OK(vkBeginCommandBuffer(cb[f], &bi));
		vkCmdFillBuffer(cb[f], buf, fills[f].offset, fills[f].size, fills[f].value);
		OK(vkEndCommandBuffer(cb[f]));
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb[f] };
		OK(vkQueueSubmit(q, 1, &si, fence));
		OK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 3000000000ull));
		OK(vkResetFences(dev, 1, &fence));
		const uint32_t *d = (const uint32_t *)((const char *)p + fills[f].offset);
		VkDeviceSize i = 0;
		while (i < fills[f].size / 4 && d[i] == fills[f].value)
			i++;
		const int filled = i == fills[f].size / 4;
		printf("fill by %s, %llu bytes: %s", fills[f].by, (unsigned long long)fills[f].size, filled ? "ok\n" : "NOT FILLED");
		if (!filled)
			printf(" (dword %llu is 0x%08x, not 0x%08x)\n", (unsigned long long)i, d[i], fills[f].value);
		wrong += !filled;
	}
	const int untouched = *(const uint32_t *)((const char *)p + 1024) == 0x11111111;
	printf("the bytes between the two fills: %s\n", untouched ? "untouched, ok" : "CHANGED");
	wrong += !untouched;

	/* "fault": work that touches memory which is not there, and what it does to the work after it. A second buffer
	 * whose memory is freed between recording and submitting (not valid Vulkan, on purpose: it is what a wrong
	 * program does); a fill into it, then a copy out of it into the good buffer. After each, fills of the good
	 * buffer, a small one and a large one, which have to land.
	 * What is known from the card (2026-10-07) is only about the command processor's own fill and copy, which the
	 * first two versions of this used without meaning to: neither stops the device, and the run after one with the
	 * copy found its first two fills not done. Now the fill and the copy are large enough to be shaders, and the
	 * fills after them say whether later work is lost, and for how long. Ends here. */
	if (argc > 2 && !strcmp(argv[2], "fault")) {
		D(vkCmdCopyBuffer)
		VkBufferCreateInfo gci = bci;
		gci.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		VkBuffer gone; OK(vkCreateBuffer(dev, &gci, 0, &gone));
		VkDeviceMemory gmem; OK(vkAllocateMemory(dev, &mai, 0, &gmem));
		OK(vkBindBufferMemory(dev, gone, gmem, 0));
		/* The steps, all recorded before the memory goes. A probe: 1024 bytes at 0 (the command processor) and 128 KiB at 8192
		 * (a compute shader), each step's own value. The fill and the copy through the freed buffer are large: shaders. */
		enum { kFillGone, kCopyGone, kProbe };
		static const struct { int kind; long sleepMs; const char *what; } steps[] = {
			{ kProbe, 0, "before anything" }, { kFillGone, 0, "a shader's fill into memory that is gone" }, { kProbe, 0, "right after it" },
			{ kCopyGone, 0, "a shader's copy out of memory that is gone" }, { kProbe, 0, "right after it" }, { kProbe, 10, "10 ms later" },
			{ kProbe, 100, "100 ms later" }, { kProbe, 1000, "1 s later" }, { kProbe, 3000, "3 s later" } };
		enum { kSteps = sizeof(steps) / sizeof(steps[0]) };
		VkCommandBufferAllocateInfo gai = cai;
		gai.commandBufferCount = kSteps;
		VkCommandBuffer gcb[kSteps]; OK(vkAllocateCommandBuffers(dev, &gai, gcb));
		const VkBufferCopy region = { 4096, 4096, kBytes - 4096 };
		for (int i = 0; i < kSteps; i++) {
			VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			if (vkBeginCommandBuffer(gcb[i], &bi) < 0) return 1;
			if (steps[i].kind == kFillGone)
				vkCmdFillBuffer(gcb[i], gone, region.srcOffset, region.size, 0xdeadbeef);
			else if (steps[i].kind == kCopyGone)
				vkCmdCopyBuffer(gcb[i], gone, buf, 1, &region);
			else {
				vkCmdFillBuffer(gcb[i], buf, 0, 1024, 0x51000000u + (uint32_t)i);
				vkCmdFillBuffer(gcb[i], buf, 8192, 1 << 17, 0x52000000u + (uint32_t)i);
			}
			if (vkEndCommandBuffer(gcb[i]) < 0) return 1;
		}
		vkFreeMemory(dev, gmem, 0);
		const uint32_t *d = (const uint32_t *)p;
		int bad = 0;
		for (int i = 0; i < kSteps; i++) {
			if (steps[i].sleepMs) {
				const struct timespec nap = { steps[i].sleepMs / 1000, steps[i].sleepMs % 1000 * 1000000L };
				nanosleep(&nap, 0);
			}
			VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &gcb[i] };
			struct timespec t0, t1;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			const VkResult submitted = vkQueueSubmit(q, 1, &si, fence);
			const VkResult waited = submitted < 0 ? submitted : vkWaitForFences(dev, 1, &fence, VK_TRUE, 15000000000ull);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			const double seconds = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
			if (waited != VK_SUCCESS) {
				printf("fault: %s: submit -> %d, wait -> %d after %.3f s: %s\n", steps[i].what, submitted, waited, seconds,
				       waited == VK_ERROR_DEVICE_LOST ? "THE DEVICE IS LOST" : "NOT FINISHED");
				bad++;
				break;
			}
			vkResetFences(dev, 1, &fence);
			if (steps[i].kind == kProbe) {
				const int small = d[0] == 0x51000000u + (uint32_t)i && d[255] == 0x51000000u + (uint32_t)i;
				const int large = d[2048] == 0x52000000u + (uint32_t)i && d[2048 + (1 << 15) - 1] == 0x52000000u + (uint32_t)i;
				printf("fault:   fills of good memory, %s: the command processor's %s, the shader's %s\n", steps[i].what,
				       small ? "ok" : "NOT DONE", large ? "ok" : "NOT DONE");
				bad += !small + !large;
			} else if (steps[i].kind == kCopyGone)
				printf("fault: %s: finished in %.3f s; it brought back 0x%08x ... 0x%08x (the fill wrote 0xdeadbeef, the buffer held "
				       "0x%08x there)\n", steps[i].what, seconds, d[1024], d[kBytes / 4 - 1], 0x0badf00d);
			else
				printf("fault: %s: finished in %.3f s\n", steps[i].what, seconds);
		}
		printf("fault: %s\n", bad ? "FAILED: work after the faults was not done" : "nothing after the faults was lost: ok");
		fflush(stdout);
		_Exit(bad ? 1 : 0);
	}

	/* A picture: a red triangle over the upper-left half of a 64x64 image cleared to blue, drawn with two
	 * shaders the driver compiles, then copied into the buffer and looked at. */
	enum { kSide = 64 };
	const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
	const VkImageSubresourceRange whole = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkImageCreateInfo imi = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
		.extent = { kSide, kSide, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
	VkImage image; OK(vkCreateImage(dev, &imi, 0, &image));
	VkMemoryRequirements imr; vkGetImageMemoryRequirements(dev, image, &imr);
	uint32_t itype = 0;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((imr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { itype = i; break; }
	VkMemoryAllocateInfo imai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, imr.size, itype };
	VkDeviceMemory imem; OK(vkAllocateMemory(dev, &imai, 0, &imem));
	OK(vkBindImageMemory(dev, image, imem, 0));
	VkImageViewCreateInfo ivi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format, .subresourceRange = whole };
	VkImageView view; OK(vkCreateImageView(dev, &ivi, 0, &view));

	VkShaderModuleCreateInfo vsi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(vkprobe_vert), vkprobe_vert };
	VkShaderModuleCreateInfo fsi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(vkprobe_frag), vkprobe_frag };
	VkShaderModule vs, fs; OK(vkCreateShaderModule(dev, &vsi, 0, &vs)); OK(vkCreateShaderModule(dev, &fsi, 0, &fs));
	VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPipelineLayout layout; OK(vkCreatePipelineLayout(dev, &pli, 0, &layout));
	const VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
	const VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	const VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	const VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
		.scissorCount = 1 };
	const VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
	const VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	const VkPipelineColorBlendAttachmentState att = { .colorWriteMask = 0xf };
	const VkPipelineColorBlendStateCreateInfo blend = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &att };
	const VkDynamicState dynamic[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	const VkPipelineDynamicStateCreateInfo dyn = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
		.pDynamicStates = dynamic };
	const VkPipelineRenderingCreateInfo target = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
		.pColorAttachmentFormats = &format };
	const VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &target, .stageCount = 2,
		.pStages = stages, .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
		.pMultisampleState = &ms, .pColorBlendState = &blend, .pDynamicState = &dyn, .layout = layout };
	VkPipeline pipeline; OK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, 0, &pipeline));

	VkCommandBufferBeginInfo tbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	OK(vkBeginCommandBuffer(cb[2], &tbi));
	VkImageMemoryBarrier toTarget = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = whole };
	vkCmdPipelineBarrier(cb[2], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, 0, 0, 0, 1, &toTarget);
	const VkRenderingAttachmentInfo colour = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = { .color = { .float32 = { 0, 0, 1, 1 } } } };
	const VkRenderingInfo rendering = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { kSide, kSide } }, .layerCount = 1,
		.colorAttachmentCount = 1, .pColorAttachments = &colour };
	vkCmdBeginRendering(cb[2], &rendering);
	vkCmdBindPipeline(cb[2], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	const VkViewport viewport = { 0, 0, kSide, kSide, 0, 1 };
	vkCmdSetViewport(cb[2], 0, 1, &viewport);
	vkCmdSetScissor(cb[2], 0, 1, &rendering.renderArea);
	vkCmdDraw(cb[2], 3, 1, 0, 0);
	vkCmdEndRendering(cb[2]);
	VkImageMemoryBarrier toSource = toTarget;
	toSource.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	toSource.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkCmdPipelineBarrier(cb[2], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &toSource);
	const VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { kSide, kSide, 1 } };
	vkCmdCopyImageToBuffer(cb[2], image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
	OK(vkEndCommandBuffer(cb[2]));
	VkSubmitInfo tsi = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb[2] };
	OK(vkQueueSubmit(q, 1, &tsi, fence));
	OK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 3000000000ull));
	/* Bytes are R, G, B, A: red is 0xff0000ff as a little-endian dword, the blue it was cleared to 0xffff0000. */
	const uint32_t *px = (const uint32_t *)p;
	uint32_t red = 0, blue = 0, other = 0;
	for (uint32_t i = 0; i < kSide * kSide; i++) {
		red += px[i] == 0xff0000ffu;
		blue += px[i] == 0xffff0000u;
		other += px[i] != 0xff0000ffu && px[i] != 0xffff0000u;
	}
	/* The triangle covers half of the picture; where exactly the diagonal's pixels fall is the rasteriser's. */
	const int drawn = px[8 * kSide + 8] == 0xff0000ffu && px[56 * kSide + 56] == 0xffff0000u && !other &&
	                  red > kSide * kSide / 2 - kSide && red < kSide * kSide / 2 + kSide;
	printf("triangle: %u red, %u blue, %u other of %u pixels; (8,8) 0x%08x, (56,56) 0x%08x: %s\n", red, blue, other, kSide * kSide,
	       px[8 * kSide + 8], px[56 * kSide + 56], drawn ? "ok" : "NOT AS DRAWN");
	wrong += !drawn;

	/* "show [seconds]" after the library's path: a picture on the boot display. The display's plane is taken
	 * through the functions the driver exports for that (radv_darwin_scanout_*), a triangle slides over a dark
	 * blue ground, each frame drawn into an image of the display's size, copied into one of two buffers the
	 * display can read and shown at a vertical blank, and the desktop is given back at the end. */
	if (argc > 2 && !strcmp(argv[2], "show")) {
		const double seconds = argc > 3 ? atof(argv[3]) : 5;
		int (*scanQuery)(VkDevice, struct n48n_scan_query *) = dlsym(lib, "radv_darwin_scanout_query");
		int (*scanAcquire)(VkDevice, uint64_t *, uint64_t *) = dlsym(lib, "radv_darwin_scanout_acquire");
		int (*scanRegister)(VkDevice, VkDeviceMemory, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t *, uint64_t *) =
			dlsym(lib, "radv_darwin_scanout_register");
		int (*scanPresent)(VkDevice, uint32_t, uint64_t *) = dlsym(lib, "radv_darwin_scanout_present");
		int (*scanRelease)(VkDevice, uint64_t *) = dlsym(lib, "radv_darwin_scanout_release");
		int bad = !scanQuery || !scanAcquire || !scanRegister || !scanPresent || !scanRelease, taken = 0;
#define Q(call) do { if (!bad) { int r_ = (int)(call); if (r_ < 0) { printf("%s -> %d\n", #call, r_); bad = 1; } } } while (0)
		struct n48n_scan_query sq;
		memset(&sq, 0, sizeof(sq));
		Q(scanQuery(dev, &sq));
		const uint32_t w = sq.plane_w, h = sq.plane_h;
		const VkDeviceSize bytes = (VkDeviceSize)sq.pitch_px * 4 * h;
		printf("display: %ux%u, pitch %u pixels, pipe %u, showing 0x%llx\n", w, h, sq.pitch_px, sq.otg, (unsigned long long)sq.plane_mc);
		bad |= !w || !h || !(sq.flags & N48N_SCANQ_GEOM_OK);

		/* The display's bytes are B, G, R, A. */
		const VkFormat shown = VK_FORMAT_B8G8R8A8_UNORM;
		VkImage simage = VK_NULL_HANDLE; VkDeviceMemory smem = VK_NULL_HANDLE, bmem[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
		VkImageView sview = VK_NULL_HANDLE; VkPipeline spipeline = VK_NULL_HANDLE; VkBuffer sbuf[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
		VkCommandPool spool = VK_NULL_HANDLE; VkCommandBuffer scb = VK_NULL_HANDLE;
		imi.format = shown; imi.extent.width = w; imi.extent.height = h;
		Q(vkCreateImage(dev, &imi, 0, &simage));
		if (!bad) {
			vkGetImageMemoryRequirements(dev, simage, &imr);
			VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, imr.size, itype };
			Q(vkAllocateMemory(dev, &ai, 0, &smem));
			Q(vkBindImageMemory(dev, simage, smem, 0));
			ivi.image = simage; ivi.format = shown;
			Q(vkCreateImageView(dev, &ivi, 0, &sview));
		}
		VkPipelineRenderingCreateInfo starget = target;
		starget.pColorAttachmentFormats = &shown;
		VkGraphicsPipelineCreateInfo sgpi = gpi;
		sgpi.pNext = &starget;
		Q(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &sgpi, 0, &spipeline));
		/* Two buffers to show in turn: VRAM, which need not be within the CPU's reach. */
		uint32_t slot[2] = { 0, 0 };
		for (int i = 0; i < 2 && !bad; i++) {
			VkBufferCreateInfo sbi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT };
			Q(vkCreateBuffer(dev, &sbi, 0, &sbuf[i]));
			if (bad)
				break;
			VkMemoryRequirements br; vkGetBufferMemoryRequirements(dev, sbuf[i], &br);
			uint32_t btype = 0;
			for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
				if ((br.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { btype = t; break; }
			VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, br.size, btype };
			Q(vkAllocateMemory(dev, &ai, 0, &bmem[i]));
			Q(vkBindBufferMemory(dev, sbuf[i], bmem[i], 0));
		}
		VkCommandPoolCreateInfo spi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, fam };
		Q(vkCreateCommandPool(dev, &spi, 0, &spool));
		VkCommandBufferAllocateInfo sai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, spool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
		Q(vkAllocateCommandBuffers(dev, &sai, &scb));

		uint64_t desktop = 0, frame0 = 0, where = 0, out3[3] = { 0, 0, 0 }, firstFrame = 0;
		Q(scanAcquire(dev, &desktop, &frame0));
		taken = !bad;
		for (int i = 0; i < 2; i++)
			Q(scanRegister(dev, bmem[i], 0, sq.pitch_px * 4, w, h, &slot[i], &where));
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		t1 = t0;
		uint32_t frames = 0;
		const uint32_t side = h / 2;
		while (!bad && (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9 < seconds) {
			const uint32_t i = frames & 1;
			VkCommandBufferBeginInfo sbegin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			Q(vkBeginCommandBuffer(scb, &sbegin));
			if (bad)
				break;
			VkImageMemoryBarrier in = toTarget, back = toSource;
			in.image = back.image = simage;
			vkCmdPipelineBarrier(scb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, 0, 0, 0, 1, &in);
			const VkRenderingAttachmentInfo ground = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = sview,
				.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { 0, 0, 0.25f, 1 } } } };
			const VkRenderingInfo whole_ = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { w, h } }, .layerCount = 1,
				.colorAttachmentCount = 1, .pColorAttachments = &ground };
			vkCmdBeginRendering(scb, &whole_);
			vkCmdBindPipeline(scb, VK_PIPELINE_BIND_POINT_GRAPHICS, spipeline);
			const VkViewport sliding = { (float)((frames * 8) % (w - side)), (float)(h / 4), (float)side, (float)side, 0, 1 };
			vkCmdSetViewport(scb, 0, 1, &sliding);
			vkCmdSetScissor(scb, 0, 1, &whole_.renderArea);
			vkCmdDraw(scb, 3, 1, 0, 0);
			vkCmdEndRendering(scb);
			vkCmdPipelineBarrier(scb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &back);
			const VkBufferImageCopy rows = { .bufferRowLength = sq.pitch_px, .bufferImageHeight = h,
				.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
			vkCmdCopyImageToBuffer(scb, simage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sbuf[i], 1, &rows);
			Q(vkEndCommandBuffer(scb));
			VkSubmitInfo ssi = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &scb };
			Q(vkQueueSubmit(q, 1, &ssi, fence));
			Q(vkWaitForFences(dev, 1, &fence, VK_TRUE, 3000000000ull));
			Q(vkResetFences(dev, 1, &fence));
			Q(scanPresent(dev, slot[i], out3));
			if (!bad && !frames++)
				firstFrame = out3[1];
			clock_gettime(CLOCK_MONOTONIC, &t1);
		}
		uint64_t given[2] = { 0, 0 };
		if (taken && scanRelease)
			(void)scanRelease(dev, given);
		const double took = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		const int showed = !bad && frames > 1 && given[0] == 1 && given[1] == desktop;
		printf("show: %u frames in %.2f s (%.1f a second), the display counted %llu; the desktop (0x%llx) given back: %s: %s\n", frames,
		       took, took > 0 ? frames / took : 0, (unsigned long long)((out3[1] - firstFrame) & 0xffffff), (unsigned long long)desktop,
		       given[0] == 1 && given[1] == desktop ? "yes" : "NO", showed ? "ok" : "FAILED");
		wrong += !showed;
		OK(vkDeviceWaitIdle(dev));
		vkDestroyCommandPool(dev, spool, 0); vkDestroyPipeline(dev, spipeline, 0); vkDestroyImageView(dev, sview, 0);
		vkDestroyImage(dev, simage, 0); vkFreeMemory(dev, smem, 0);
		for (int i = 0; i < 2; i++) { vkDestroyBuffer(dev, sbuf[i], 0); vkFreeMemory(dev, bmem[i], 0); }
#undef Q
	} else {
		printf("the display was not asked for: add \"show\" after the library's path to put a picture on it\n");
	}

	OK(vkDeviceWaitIdle(dev));
	vkDestroyPipeline(dev, pipeline, 0); vkDestroyPipelineLayout(dev, layout, 0); vkDestroyShaderModule(dev, vs, 0);
	vkDestroyShaderModule(dev, fs, 0); vkDestroyImageView(dev, view, 0); vkDestroyImage(dev, image, 0); vkFreeMemory(dev, imem, 0);
	vkDestroyFence(dev, fence, 0); vkDestroyCommandPool(dev, pool, 0); vkDestroyBuffer(dev, buf, 0); vkFreeMemory(dev, mem, 0);
	vkDestroyDevice(dev, 0);
	/* VKPROBE_NOGPU=1: nothing executes (a stand-in for the kext), so neither the fills nor the picture are expected. */
	printf("%s\n", !wrong ? "done: all ok" : getenv("VKPROBE_NOGPU") ? "done (no GPU behind it: fills and picture were not expected)" : "FAILED");
	return wrong && !getenv("VKPROBE_NOGPU");
}
