// The Metal compute kernels the fake paravirtual GPU's host runs on the card (M3, docs/m1-stream.md s.5b), translated by
// tools/metal/m3/build.sh into src/mtlkernels.h. The host picks one by the function's name, so each must match, character for
// character in its signature, the kernel tools/metal/m1/mtlprobe.m compiles on the guest.
#include <metal_stdlib>
using namespace metal;

kernel void vadd(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]],
                 device float *c [[buffer(2)]], uint i [[thread_position_in_grid]]) { c[i] = a[i] + b[i]; }

kernel void grid2d(device uint *out [[buffer(0)]], uint2 p [[thread_position_in_grid]], uint2 n [[threads_per_grid]]) {
	out[p.y * n.x + p.x] = p.x + 1000 * p.y;
}

// Not a guest's: the host's own resolve of a render pass's multisampled colour attachment (src/pvgpu.cpp resolveMsaa), the average of its
// samples into the attachment's texture as one sample (the colour's bytes averaged as they are: rdna4_resolve_srgb for an sRGB target).
kernel void rdna4_resolve(texture2d_ms<float, access::read> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],
                          uint2 p [[thread_position_in_grid]]) {
	if (p.x >= dst.get_width() || p.y >= dst.get_height())
		return;
	const uint n = src.get_num_samples();
	float4 sum = 0;
	for (uint i = 0; i < n; i++)
		sum += src.read(p, i);
	dst.write(sum / float(n), p);
}

// An sRGB target's: its samples decoded, averaged in linear light, encoded again (as Metal resolves one: averaging the encoded bytes darkens
// and roughens edges). The texture is read and written as plain 8_8_8_8 unorm; alpha is not encoded.
static float3 srgbToLinear(float3 c) { return select(pow((c + 0.055f) / 1.055f, 2.4f), c / 12.92f, c <= 0.04045f); }
static float3 linearToSrgb(float3 c) { return select(1.055f * pow(c, 1.0f / 2.4f) - 0.055f, c * 12.92f, c <= 0.0031308f); }

kernel void rdna4_resolve_srgb(texture2d_ms<float, access::read> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],
                               uint2 p [[thread_position_in_grid]]) {
	if (p.x >= dst.get_width() || p.y >= dst.get_height())
		return;
	const uint n = src.get_num_samples();
	float4 sum = 0;
	for (uint i = 0; i < n; i++) {
		const float4 c = src.read(p, i);
		sum += float4(srgbToLinear(c.rgb), c.a);
	}
	sum /= float(n);
	dst.write(float4(linearToSrgb(sum.rgb), sum.a), p);
}

// Generate one linear mip level. Match the native Metal driver's existing CPU filter:
// nearest for R16, and rounded 2x2 averages of the four stored bytes for RG16.
kernel void rdna4_mip_planes(device const uint *src [[buffer(0)]], device uint *dst [[buffer(1)]],
                             constant uint *layout [[buffer(2)]], uint2 p [[thread_position_in_grid]])
{
    const uint w = layout[0], h = layout[1], sourceHeight = layout[2], sourceWidth = layout[3];
    const uint srcPitch = layout[4], dstPitch = layout[5];
    if (p.y >= h || p.x >= w)
        return;
    const uint top = p.y * 2 * srcPitch;
    if (layout[6] == 2) {
        const uint x = p.x * 2;
        if (x >= w)
            return;
        uint packed = src[top + x] & 0xffffu;
        if (x + 1 < w)
            packed |= src[top + x + 1] << 16;
        dst[p.y * dstPitch + p.x] = packed;
        return;
    }
    const uint bottom = min(p.y * 2 + 1, sourceHeight - 1) * srcPitch;
    const uint left = p.x * 2, right = min(left + 1, sourceWidth - 1);
    const uint a = src[top + left], b = src[top + right], c = src[bottom + left], d = src[bottom + right];
    const uint mask = 0x00ff00ffu;
    const uint even = ((a & mask) + (b & mask) + (c & mask) + (d & mask) + 0x00020002u) >> 2;
    const uint odd = (((a >> 8) & mask) + ((b >> 8) & mask) + ((c >> 8) & mask) + ((d >> 8) & mask) + 0x00020002u) >> 2;
    dst[p.y * dstPitch + p.x] = (even & mask) | ((odd & mask) << 8);
}

kernel void rdna4_copy_plane(device const uint4 *src [[buffer(0)]], device uint4 *dst [[buffer(1)]],
                             constant uint *layout [[buffer(2)]], uint2 p [[thread_position_in_grid]])
{
    if (p.x < layout[0] && p.y < layout[1])
        dst[p.y * layout[3] + p.x] = src[p.y * layout[2] + p.x];
}
