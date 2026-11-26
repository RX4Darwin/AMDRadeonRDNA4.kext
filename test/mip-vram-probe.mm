#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <time.h>
#include <objc/runtime.h>
#include <sys/stat.h>
#include <vector>

@protocol ProbeDevice <MTLDevice>
- (instancetype)initNativeWithAcceleratorPort:(io_service_t)service;
@end
@protocol ProbeTexture <MTLTexture>
- (BOOL)mipInVram:(NSUInteger)level;
- (id<MTLBuffer>)memory;
@end

static int test(id<MTLDevice> dev, const char *name, MTLPixelFormat format, unsigned w, unsigned h, unsigned bpp, bool vram = true)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:YES];
    d.storageMode = MTLStorageModeManaged;
    id<ProbeTexture> texture = (id<ProbeTexture>)[dev newTextureWithDescriptor:d];
    const unsigned baseBytes = ((w * bpp + 255) & ~255u) * h;
    if (!texture || [texture mipInVram:0] || [texture mipInVram:1] != vram ||
        texture.storageMode != MTLStorageModeManaged ||
        (vram ? texture.memory.length != baseBytes : texture.memory.length <= baseBytes)) {
        fprintf(stderr, "%s: incorrect backing (texture=%p, host=%lu)\n", name, texture, texture.memory.length);
        return 1;
    }
    std::vector<uint8_t> prev(w * h * bpp);
    uint32_t seed = 0xf134fa57u;
    for (uint8_t &p : prev) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        p = seed >> 24;
    }
    [texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:prev.data() bytesPerRow:w * bpp];
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit generateMipmapsForTexture:texture];
    [blit endEncoding];
    const uint64_t start = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    [cmd commit];
    [cmd waitUntilCompleted];
    const double duration = (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - start) / 1e6;
    if (cmd.status != MTLCommandBufferStatusCompleted) {
        fprintf(stderr, "%s: GPU generate failed %s\n", name, cmd.error.description.UTF8String);
        return 1;
    }
    for (NSUInteger l = 1; l < texture.mipmapLevelCount; l++) {
        const unsigned pw = w, ph = h;
        w = MAX(w / 2, 1u), h = MAX(h / 2, 1u);
        std::vector<uint8_t> ref(w * h * bpp), actual(ref.size());
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++)
                for (unsigned c = 0; c < bpp; c++) {
                    const unsigned ax = 2 * x, bx = MIN(ax + 1, pw - 1), ay = 2 * y, by = MIN(ay + 1, ph - 1);
                    const unsigned a = prev[(ay * pw + ax) * bpp + c];
                    const unsigned sum = a + prev[(ay * pw + bx) * bpp + c] +
                                         prev[(by * pw + ax) * bpp + c] + prev[(by * pw + bx) * bpp + c];
                    ref[(y * w + x) * bpp + c] = bpp == 2 ? a : (sum + 2) / 4;
                }
        [texture getBytes:actual.data() bytesPerRow:w * bpp fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:l];
        if (ref != actual) {
            size_t i = 0;
            while (ref[i] == actual[i]) i++;
            fprintf(stderr, "%s: level %lu at %zu expected %u read %u\n", name, l, i, ref[i], actual[i]);
            return 1;
        }
        if (l == 1) {
            id<MTLTexture> view = [texture newTextureViewWithPixelFormat:format textureType:MTLTextureType2D
                                                                   levels:NSMakeRange(1, 1) slices:NSMakeRange(0, 1)];
            MTLRegion patch = MTLRegionMake2D(1, 7, 3, 2);
            uint8_t changed[24] = {}, filtered[24] = {};
            memset(changed, 0xa7, sizeof changed);
            [view replaceRegion:patch mipmapLevel:0 withBytes:changed bytesPerRow:3 * bpp];
            [texture getBytes:filtered bytesPerRow:3 * bpp fromRegion:patch mipmapLevel:1];
            if (!view || memcmp(changed, filtered, 6 * bpp)) {
                fprintf(stderr, "%s: VRAM mip view update did not reach parent\n", name);
                return 1;
            }
            [texture getBytes:actual.data() bytesPerRow:w * bpp fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:1];
            auto expected = ref;
            for (unsigned y = 7; y < 9; y++)
                memset(expected.data() + (y * w + 1) * bpp, 0xa7, 3 * bpp);
            if (actual != expected) {
                fprintf(stderr, "%s: view update changed neighboring texels\n", name);
                return 1;
            }
            memset(filtered, 0, sizeof filtered);
            const NSUInteger rowBytes = 256;
            id<MTLBuffer> buffer = [dev newBufferWithLength:rowBytes * 2 options:MTLResourceStorageModeShared];
            memset(buffer.contents, 0x42, rowBytes * 2);
            id<MTLBuffer> result = [dev newBufferWithLength:rowBytes * 2 options:MTLResourceStorageModeShared];
            id<MTLTexture> dst = [dev newTextureWithDescriptor:[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                width:32 height:32 mipmapped:NO]];
            cmd = [queue commandBuffer];
            blit = [cmd blitCommandEncoder];
            [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:rowBytes sourceBytesPerImage:0 sourceSize:patch.size
                       toTexture:view destinationSlice:0 destinationLevel:0 destinationOrigin:patch.origin];
            [blit copyFromTexture:view sourceSlice:0 sourceLevel:0 sourceOrigin:patch.origin sourceSize:patch.size
                         toTexture:dst destinationSlice:0 destinationLevel:0 destinationOrigin:patch.origin];
            [blit copyFromTexture:view sourceSlice:0 sourceLevel:0 sourceOrigin:patch.origin sourceSize:patch.size
                          toBuffer:result destinationOffset:0 destinationBytesPerRow:rowBytes destinationBytesPerImage:0];
            [blit endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];
            [dst getBytes:filtered bytesPerRow:3 * bpp fromRegion:patch mipmapLevel:0];
            if (cmd.status != MTLCommandBufferStatusCompleted ||
                memcmp(result.contents, buffer.contents, 3 * bpp) ||
                memcmp((uint8_t *)result.contents + rowBytes, (uint8_t *)buffer.contents + rowBytes, 3 * bpp) ||
                memcmp(filtered, buffer.contents, 6 * bpp)) {
                fprintf(stderr, "%s: tail blit failed (%s)\n", name, cmd.error.description.UTF8String);
                return 1;
            }
        }
        prev.swap(ref);
    }
    printf("%s: %lu %s levels bit-identical, host backing %lu bytes, view and blits passed, generate %.3f ms\n",
           name, texture.mipmapLevelCount - 1, vram ? "VRAM" : "host", texture.memory.length, duration);
    return 0;
}

int main(int argc, const char **argv)
{
    @autoreleasepool {
        if (argc != 2) {
            fprintf(stderr, "usage: %s <native Metal bundle executable>\n", argv[0]);
            return 1;
        }
        if (!dlopen(argv[1], RTLD_NOW | RTLD_LOCAL)) {
            fprintf(stderr, "load: %s\n", dlerror());
            return 1;
        }
        Class cls = NSClassFromString(@"RDNA4MTLDevice");
        const char *image = cls ? class_getImageName(cls) : nullptr;
        struct stat requested{}, loaded{};
        if (!image || stat(argv[1], &requested) || stat(image, &loaded) ||
            requested.st_dev != loaded.st_dev || requested.st_ino != loaded.st_ino) {
            fprintf(stderr, "the device class did not come from the requested executable\n");
            return 1;
        }
        io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RDNA4Accelerator"));
        if (!service) {
            fprintf(stderr, "no RDNA4 accelerator service\n");
            return 1;
        }
        id<ProbeDevice> dev = [[cls alloc] initNativeWithAcceleratorPort:service];
        IOObjectRelease(service);
        if (!dev) {
            fprintf(stderr, "cannot open native RDNA4 Metal device\n");
            return 1;
        }
        return test(dev, "R16 3840x2160", MTLPixelFormatR16Unorm, 3840, 2160, 2) ||
               test(dev, "RG16 1920x1080", MTLPixelFormatRG16Unorm, 1920, 1080, 4) ||
               test(dev, "RG16 2049x1025", MTLPixelFormatRG16Unorm, 2049, 1025, 4) ||
               test(dev, "RG16 129x65", MTLPixelFormatRG16Unorm, 129, 65, 4, false);
    }
}
