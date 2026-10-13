//
//  swapprobe.c
//  AMDRadeonRDNA4
//
//  A Vulkan program as an application writes it, with a surface and a swapchain, where vkprobe hands its pictures
//  to the display itself: a headless surface (VK_EXT_headless_surface), a swapchain of the display's size, a
//  triangle moving over a background that changes colour, vkQueuePresentKHR for every frame. With mesa-patches/0008
//  the driver shows a headless swapchain's images on the display the kext lends; this is the test of that, and the
//  step before a program that somebody else wrote.
//
//    make mesa     (builds it with the driver and vkprobe)
//    sudo build/swapprobe build/libvulkan_radeon.dylib [seconds]
//
//  It drives the driver through its ICD entry point, as vkprobe does (no Vulkan loader). On a host, with a stand-in
//  for the kext (RADV_DARWIN_MOCK=1, or tools/n48n-host), nothing is drawn, and the calls still have to succeed.
//
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "navi48_native_abi.h"
#include "shaders/vkprobe_frag.h"
#include "shaders/vkprobe_vert.h"

typedef PFN_vkVoidFunction (*GetProc)(VkInstance, const char *);
#define F(inst, name) PFN_##name name = (PFN_##name)gip(inst, #name); if (!name) { printf("no %s\n", #name); return 1; }
#define OK(call) do { VkResult r_ = (call); if (r_ < 0) { printf("%-34s -> %d\n", #call, r_); return 1; } } while (0)

static double seconds(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
	void *lib = dlopen(argc > 1 ? argv[1] : "libvulkan_radeon.dylib", RTLD_NOW);
	if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
	GetProc gip = (GetProc)dlsym(lib, "vk_icdGetInstanceProcAddr");
	if (!gip) { printf("no vk_icdGetInstanceProcAddr\n"); return 1; }
	const double length = argc > 2 ? atof(argv[2]) : 5;

	F(NULL, vkCreateInstance)
	const char *instanceExtensions[] = { "VK_KHR_surface", "VK_EXT_headless_surface" };
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "swapprobe", 1, "none", 0, VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app, 0, 0, 2, instanceExtensions };
	VkInstance inst; OK(vkCreateInstance(&ici, 0, &inst));
	F(inst, vkEnumeratePhysicalDevices) F(inst, vkCreateDevice) F(inst, vkGetDeviceProcAddr) F(inst, vkCreateHeadlessSurfaceEXT)
	F(inst, vkGetPhysicalDeviceSurfaceCapabilitiesKHR) F(inst, vkGetPhysicalDeviceSurfaceFormatsKHR)
	uint32_t n = 1; VkPhysicalDevice pd;
	OK(vkEnumeratePhysicalDevices(inst, &n, &pd));
	if (!n) { printf("no device\n"); return 2; }
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
	const char *deviceExtensions[] = { "VK_KHR_swapchain" };
	VkPhysicalDeviceVulkan13Features v13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &v13, 0, 1, &qci, 0, 0, 1, deviceExtensions };
	VkDevice dev; OK(vkCreateDevice(pd, &dci, 0, &dev));
#define D(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name); if (!name) { printf("no %s\n", #name); return 1; }
	D(vkGetDeviceQueue) D(vkCreateSwapchainKHR) D(vkGetSwapchainImagesKHR) D(vkAcquireNextImageKHR) D(vkQueuePresentKHR)
	D(vkDestroySwapchainKHR) D(vkCreateImageView) D(vkCreateShaderModule) D(vkCreatePipelineLayout) D(vkCreateGraphicsPipelines)
	D(vkCreateCommandPool) D(vkAllocateCommandBuffers) D(vkBeginCommandBuffer) D(vkEndCommandBuffer) D(vkCmdPipelineBarrier)
	D(vkCmdBeginRendering) D(vkCmdEndRendering) D(vkCmdBindPipeline) D(vkCmdSetViewport) D(vkCmdSetScissor) D(vkCmdDraw)
	D(vkCreateFence) D(vkQueueSubmit) D(vkWaitForFences) D(vkResetFences) D(vkDeviceWaitIdle)
	VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

	/* The display's size is the kext's to say (a headless surface has none of its own). */
	VkExtent2D extent = { 1280, 720 };
	int (*query)(VkDevice, struct n48n_scan_query *) = (int (*)(VkDevice, struct n48n_scan_query *))dlsym(lib, "radv_darwin_scanout_query");
	struct n48n_scan_query plane;
	const int display = query && query(dev, &plane) == 0 && plane.plane_w && plane.plane_h;
	if (display)
		extent = (VkExtent2D) { plane.plane_w, plane.plane_h };
	printf("display: %s\n", display ? "the kext has one to lend" : "none (the swapchain will not be shown)");

	VkHeadlessSurfaceCreateInfoEXT hci = { VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
	VkSurfaceKHR surface; OK(vkCreateHeadlessSurfaceEXT(inst, &hci, 0, &surface));
	VkSurfaceCapabilitiesKHR caps; OK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps));
	uint32_t formats = 1; VkSurfaceFormatKHR format;
	if (vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &formats, &format) < 0 || !formats) { printf("no surface format\n"); return 1; }
	uint32_t count = caps.minImageCount < 3 ? 3 : caps.minImageCount;
	if (caps.maxImageCount && count > caps.maxImageCount)
		count = caps.maxImageCount;
	VkSwapchainCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface, .minImageCount = count,
		.imageFormat = format.format, .imageColorSpace = format.colorSpace, .imageExtent = extent, .imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
	VkSwapchainKHR swapchain; OK(vkCreateSwapchainKHR(dev, &sci, 0, &swapchain));
	enum { kMaxImages = 8 };
	uint32_t images = kMaxImages; VkImage image[kMaxImages]; VkImageView view[kMaxImages];
	OK(vkGetSwapchainImagesKHR(dev, swapchain, &images, image));
	printf("swapchain: %ux%u, format %d, %u images\n", extent.width, extent.height, format.format, images);
	const VkImageSubresourceRange whole = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	for (uint32_t i = 0; i < images; i++) {
		VkImageViewCreateInfo ivi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = format.format, .subresourceRange = whole };
		OK(vkCreateImageView(dev, &ivi, 0, &view[i]));
	}

	/* vkprobe's triangle (shaders/vkprobe.vert and .frag), in the swapchain's format. */
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
		.pColorAttachmentFormats = &format.format };
	const VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &target, .stageCount = 2,
		.pStages = stages, .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
		.pMultisampleState = &ms, .pColorBlendState = &blend, .pDynamicState = &dyn, .layout = layout };
	VkPipeline pipeline; OK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, 0, &pipeline));

	VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
	VkCommandPool pool; OK(vkCreateCommandPool(dev, &cpi, 0, &pool));
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
	VkCommandBuffer cb; OK(vkAllocateCommandBuffers(dev, &cai, &cb));
	VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence; OK(vkCreateFence(dev, &fci, 0, &fence));

	/* Frames: the triangle's square, a third of the display's height, travels to the right and comes round; the
	 * background goes from blue to green and back. One frame is drawn, waited for and presented before the next. */
	const double start = seconds();
	uint32_t frames = 0;
	const uint32_t side = extent.height / 3;
	for (double now = start; now - start < length; now = seconds(), frames++) {
		uint32_t index = 0;
		OK(vkAcquireNextImageKHR(dev, swapchain, 1000000000ull, VK_NULL_HANDLE, VK_NULL_HANDLE, &index));
		VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		OK(vkBeginCommandBuffer(cb, &bi));
		VkImageMemoryBarrier toTarget = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image[index],
			.subresourceRange = whole };
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, 0, 0, 0, 1, &toTarget);
		const float phase = (float)((now - start) / 4 - (double)(long)((now - start) / 4));   /* 0..1 every four seconds */
		const float green = phase < 0.5f ? 2 * phase : 2 - 2 * phase;
		const VkRenderingAttachmentInfo colour = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view[index],
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.clearValue = { .color = { .float32 = { 0, green / 2, 0.5f - green / 2, 1 } } } };
		const VkRenderingInfo rendering = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, extent }, .layerCount = 1,
			.colorAttachmentCount = 1, .pColorAttachments = &colour };
		vkCmdBeginRendering(cb, &rendering);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		const VkViewport viewport = { phase * (float)(extent.width - side), (float)side, (float)side, (float)side, 0, 1 };
		vkCmdSetViewport(cb, 0, 1, &viewport);
		vkCmdSetScissor(cb, 0, 1, &rendering.renderArea);
		vkCmdDraw(cb, 3, 1, 0, 0);
		vkCmdEndRendering(cb);
		VkImageMemoryBarrier toPresent = toTarget;
		toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; toPresent.dstAccessMask = 0;
		toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &toPresent);
		OK(vkEndCommandBuffer(cb));
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb };
		OK(vkQueueSubmit(q, 1, &si, fence));
		OK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 3000000000ull));
		OK(vkResetFences(dev, 1, &fence));
		VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 0, 0, 1, &swapchain, &index };
		OK(vkQueuePresentKHR(q, &pi));
	}
	const double took = seconds() - start;
	OK(vkDeviceWaitIdle(dev));
	vkDestroySwapchainKHR(dev, swapchain, 0);     /* gives the display back */
	printf("presented: %u frames in %.2f s (%.1f a second)\n", frames, took, took > 0 ? frames / took : 0);
	printf("done: all ok\n");
	return 0;
}
