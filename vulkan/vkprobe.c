//
//  vkprobe.c
//  RDNA4FB
//
//  Drives the RADV Darwin build directly through its ICD entry point (no Vulkan loader needed): instance, the
//  one device, a buffer with memory, a command buffer that fills it, a submit and a wait.
//
//    clang -arch x86_64 -mmacosx-version-min=11.0 -std=gnu11 -I <work>/mesa/include vulkan/vkprobe.c -o vkprobe
//    RADV_DARWIN_FAKE=1 RADV_DARWIN_MOCK=1 ./vkprobe <work>/build/src/amd/vulkan/libvulkan_radeon.dylib
//
//  RADV_DARWIN_FAKE=1 makes the driver offer its device; RADV_DARWIN_MOCK=1 puts Mesa's in-process stand-in in
//  place of the kext (nothing executes, so the fill is not seen); RADV_DARWIN_TRACE=1 logs every call the
//  driver makes to the kext interface. Against a real kext, leave RADV_DARWIN_MOCK out.
//
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

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
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qci };
	VkDevice dev;
	OK(vkCreateDevice(pd[0], &dci, 0, &dev));
#define D(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name); if (!name) { printf("no %s\n", #name); return 1; }
	D(vkGetDeviceQueue) D(vkCreateBuffer) D(vkGetBufferMemoryRequirements) D(vkAllocateMemory) D(vkBindBufferMemory) D(vkMapMemory)
	D(vkCreateCommandPool) D(vkAllocateCommandBuffers) D(vkBeginCommandBuffer) D(vkCmdFillBuffer) D(vkEndCommandBuffer)
	D(vkCreateFence) D(vkQueueSubmit) D(vkWaitForFences) D(vkDeviceWaitIdle) D(vkDestroyFence) D(vkDestroyCommandPool)
	D(vkDestroyBuffer) D(vkFreeMemory) D(vkDestroyDevice)
	VkQueue q; vkGetDeviceQueue(dev, fam, 0, &q);
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 1 << 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer buf; OK(vkCreateBuffer(dev, &bci, 0, &buf));
	VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
	uint32_t type = 0;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { type = i; break; }
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, type };
	VkDeviceMemory mem; OK(vkAllocateMemory(dev, &mai, 0, &mem));
	OK(vkBindBufferMemory(dev, buf, mem, 0));
	void *p = 0; OK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &p));
	memset(p, 0x11, 64);
	VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, fam };
	VkCommandPool pool; OK(vkCreateCommandPool(dev, &cpi, 0, &pool));
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
	VkCommandBuffer cb; OK(vkAllocateCommandBuffers(dev, &cai, &cb));
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	OK(vkBeginCommandBuffer(cb, &bi));
	vkCmdFillBuffer(cb, buf, 0, 1 << 16, 0xcafef00d);
	OK(vkEndCommandBuffer(cb));
	VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence; OK(vkCreateFence(dev, &fci, 0, &fence));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb };
	OK(vkQueueSubmit(q, 1, &si, fence));
	OK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 3000000000ull));
	printf("first dword after the fill: 0x%08x (0xcafef00d only if a GPU ran it)\n", *(uint32_t *)p);
	OK(vkDeviceWaitIdle(dev));
	vkDestroyFence(dev, fence, 0); vkDestroyCommandPool(dev, pool, 0); vkDestroyBuffer(dev, buf, 0); vkFreeMemory(dev, mem, 0);
	vkDestroyDevice(dev, 0);
	printf("done\n");
	return 0;
}
