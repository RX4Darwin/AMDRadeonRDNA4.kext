// radv-triangle.c: the kext's first triangle (G3) drawn by RADV on Linux,
// so its command stream and NGG shader can be dumped and compared with ours.
//
// Same draw as src/gfxring.cpp: 256x256 R8G8B8A8 target cleared to 0, one
// triangle from VertexID (no vertex buffers), 0 -> (-0.5,-0.5),
// 1 -> (0.5,-0.5), 2 -> (0,0.5), z 0, w 1, a constant red PS. The target is
// read back and the red pixels counted (the kext wants 8192).
//
// TRI_COLOR=1 draws the G4 triangle instead: tri_col.vert / tri_col.frag, vertex 0 red, 1 green, 2 blue, interpolated by
// the PS; the readback is then checked against the exact barycentric colour of every covered pixel (see checkColour).
//
// Build and run: tools/radv-triangle/run.sh (dumps with RADV_DEBUG).

#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 256
#define H 256

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static uint32_t *readFile(const char *path, size_t *size) {
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END);
	*size = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint32_t *buf = malloc(*size);
	if (fread(buf, 1, *size, f) != *size) { perror(path); exit(1); }
	fclose(f);
	return buf;
}

static uint32_t findMemory(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no memory type for bits 0x%x flags 0x%x\n", bits, want);
	exit(1);
}

static VkShaderModule loadShader(VkDevice dev, const char *path) {
	size_t size;
	uint32_t *code = readFile(path, &size);
	VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code };
	VkShaderModule m;
	CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
	free(code);
	return m;
}

// The G4 check. Vertices in pixels: v0 (64,64) red, v1 (192,64) green, v2 (128,192) blue (viewport 256x256, NDC (-.5,-.5) (.5,-.5) (0,.5)).
// For each covered pixel (alpha 0xFF) the expected colour is 255 * barycentrics at the pixel centre. Reports the measured tolerances:
// the largest |channel - expected| over all pixels, the largest |R+G+B - 255|, and the colours at the vertex-nearest and centroid pixels.
// Returns 0 when every pixel is within `tol` of the exact barycentric colour and the vertex/centroid spot checks hold.
static int checkColour(const uint32_t *px, uint32_t covered) {
	const double x0 = 64, y0 = 64, x1 = 192, y1 = 64, x2 = 128, y2 = 192;
	const double den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
	double maxErr = 0, maxSum = 0;
	uint32_t n = 0, bad = 0, ownDominant = 0;
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			uint32_t v = px[y * W + x];
			if (!v)
				continue;
			n++;
			const double cx = x + 0.5, cy = y + 0.5;
			const double l0 = ((y1 - y2) * (cx - x2) + (x2 - x1) * (cy - y2)) / den;
			const double l1 = ((y2 - y0) * (cx - x2) + (x0 - x2) * (cy - y2)) / den;
			const double l2 = 1.0 - l0 - l1;
			const double e[3] = { 255 * l0, 255 * l1, 255 * l2 };
			const int c[3] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff };
			if ((v >> 24) != 0xff)
				bad++;
			int sum = c[0] + c[1] + c[2];
			if (fabs(sum - 255.0) > maxSum) maxSum = fabs(sum - 255.0);
			for (int k = 0; k < 3; k++)
				if (fabs(c[k] - e[k]) > maxErr) maxErr = fabs(c[k] - e[k]);
			int dom = c[0] >= c[1] && c[0] >= c[2] ? 0 : c[1] >= c[2] ? 1 : 2;
			int want = l0 >= l1 && l0 >= l2 ? 0 : l1 >= l2 ? 1 : 2;
			ownDominant += dom == want;
		}
	const uint32_t near0 = px[65 * W + 66], near1 = px[65 * W + 189], near2 = px[188 * W + 128], cen = px[106 * W + 128];
	printf("colour: %u covered pixels (want %u), %u with alpha != 0xFF\n", n, covered, bad);
	printf("colour: max |channel - exact barycentric| %.2f, max |R+G+B - 255| %.0f, dominant channel = nearest vertex's in %u/%u pixels\n",
	       maxErr, maxSum, ownDominant, n);
	printf("colour: near v0 (66,65) 0x%08x  near v1 (189,65) 0x%08x  near v2 (128,188) 0x%08x  centroid (128,106) 0x%08x (want ~85,85,85)\n",
	       near0, near1, near2, cen);
	const int ok = n == covered && !bad && maxErr <= 3.0 && maxSum <= 3.0 && ownDominant == n &&
	               (near0 & 0xff) > 200 && ((near1 >> 8) & 0xff) > 200 && ((near2 >> 16) & 0xff) > 200 &&
	               abs((int)(cen & 0xff) - 85) <= 6 && abs((int)((cen >> 8) & 0xff) - 85) <= 6 && abs((int)((cen >> 16) & 0xff) - 85) <= 6;
	printf("colour: %s\n", ok ? "THE COLOUR TRIANGLE IS RIGHT" : "wrong colour image");
	return !ok;
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : ".";
	char vsPath[4096], fsPath[4096];
	const int colour = getenv("TRI_COLOR") != NULL;
	snprintf(vsPath, sizeof(vsPath), "%s/%s.vert.spv", dir, colour ? "tri_col" : "tri");
	snprintf(fsPath, sizeof(fsPath), "%s/%s.frag.spv", dir, colour ? "tri_col" : "tri");

	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "radv-triangle", 1, NULL, 0, VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst));

	uint32_t n = 8;
	VkPhysicalDevice pds[8];
	CHECK(vkEnumeratePhysicalDevices(inst, &n, pds));
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	for (uint32_t i = 0; i < n; i++) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(pds[i], &p);
		if (p.vendorID == 0x1002) { pd = pds[i]; printf("device: %s\n", p.deviceName); break; }
	}
	if (!pd) { fprintf(stderr, "no AMD device\n"); return 1; }

	uint32_t qfCount = 16;
	VkQueueFamilyProperties qf[16];
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qf);
	uint32_t qfi = UINT32_MAX;
	for (uint32_t i = 0; i < qfCount; i++)
		if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfi, 1, &prio };
	VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13, 0, 1, &qci, 0, NULL, 0, NULL, NULL };
	VkDevice dev;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue q;
	vkGetDeviceQueue(dev, qfi, 0, &q);

	// Colour target: linear, like the kext's (a plain pitch-linear buffer).
	VkImageCreateInfo imci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkImage img;
	CHECK(vkCreateImage(dev, &imci, NULL, &img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, img, &mr);
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
		findMemory(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	VkDeviceMemory imgMem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &imgMem));
	CHECK(vkBindImageMemory(dev, img, imgMem, 0));
	VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D,
		VK_FORMAT_R8G8B8A8_UNORM, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	VkImageView view;
	CHECK(vkCreateImageView(dev, &ivci, NULL, &view));

	// Readback buffer.
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
	VkBuffer buf;
	CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = findMemory(pd, mr.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory bufMem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &bufMem));
	CHECK(vkBindBufferMemory(dev, buf, bufMem, 0));

	// Pipeline: no vertex input, no depth, no blending, no culling.
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPipelineLayout layout;
	CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
	VkShaderModule vs = loadShader(dev, vsPath), fs = loadShader(dev, fsPath);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
	};
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };
	VkViewport vp = { 0, 0, W, H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };
	VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc };
	VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f };
	VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
	VkPipelineRenderingCreateInfo prci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &fmt };
	VkGraphicsPipelineCreateInfo gpci = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &prci, .stageCount = 2, .pStages = stages,
		.pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
		.pMultisampleState = &ms, .pColorBlendState = &cb, .layout = layout,
	};
	VkPipeline pipe;
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
	if (getenv("TRI_COMPILE_ONLY")) {   // shaders dumped (RADV_DEBUG=shaders), nothing submitted to the GPU
		printf("compile only: pipeline created, nothing submitted\n");
		return 0;
	}

	// Pipeline statistics, the same counters the kext reads.
	VkQueryPoolCreateInfo qpci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_PIPELINE_STATISTICS, 1,
		VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
		VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
		VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT };
	VkQueryPool qp;
	int stats = getenv("TRI_NOSTATS") == NULL;
	if (stats)
		CHECK(vkCreateQueryPool(dev, &qpci, NULL, &qp));

	VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfi };
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
	VkCommandBuffer cmd;
	CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
	VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	CHECK(vkBeginCommandBuffer(cmd, &cbbi));
	if (stats)
		vkCmdResetQueryPool(cmd, qp, 0, 1);

	VkImageMemoryBarrier toColor = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
		img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
		0, NULL, 0, NULL, 1, &toColor);

	VkRenderingAttachmentInfo att = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { 0, 0, 0, 0 } } } };
	VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = sc, .layerCount = 1,
		.colorAttachmentCount = 1, .pColorAttachments = &att };
	vkCmdBeginRendering(cmd, &ri);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	if (stats)
		vkCmdBeginQuery(cmd, qp, 0, 0);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	if (stats)
		vkCmdEndQuery(cmd, qp, 0);
	vkCmdEndRendering(cmd);

	VkImageMemoryBarrier toSrc = toColor;
	toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	toSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		0, NULL, 0, NULL, 1, &toSrc);
	VkBufferImageCopy region = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
	vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
	CHECK(vkEndCommandBuffer(cmd));

	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL };
	CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
	CHECK(vkQueueWaitIdle(q));

	uint32_t *px;
	CHECK(vkMapMemory(dev, bufMem, 0, W * H * 4, 0, (void **)&px));
	uint32_t red = 0, other = 0, minX = W, maxX = 0, minY = H, maxY = 0;
	uint32_t coveredAny = 0;
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			uint32_t v = px[y * W + x];
			if (!v)
				continue;
			coveredAny++;
			if (v == 0xFF0000FFu) red++; else other++;
			if (x < minX) minX = x;
			if (x > maxX) maxX = x;
			if (y < minY) minY = y;
			if (y > maxY) maxY = y;
		}
	printf("pixels: %u red (0xFF0000FF, kext wants 8192), %u others; bounds x %u..%u y %u..%u\n",
		red, other, minX, maxX, minY, maxY);

	if (stats) {
		uint64_t s[6];
		CHECK(vkGetQueryPoolResults(dev, qp, 0, 1, sizeof(s), s, sizeof(s),
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
		printf("stats: ia_verts %llu ia_prims %llu vs_inv %llu c_inv %llu c_prim %llu ps_inv %llu\n",
			(unsigned long long)s[0], (unsigned long long)s[1], (unsigned long long)s[2],
			(unsigned long long)s[3], (unsigned long long)s[4], (unsigned long long)s[5]);
	}

	const int colourBad = colour ? checkColour(px, 8192) : 0;
	vkDeviceWaitIdle(dev);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	if (colour)
		return colourBad || coveredAny != 8192 ? 2 : 0;
	return red == 8192 ? 0 : 2;
}
