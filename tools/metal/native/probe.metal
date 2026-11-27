// The native-Metal spike's test functions (mtlprobe native <probe.metallib>, docs/native-metal-spike.md): gate 2's kernels, gate 3's
// triangle (the kext's colour triangle: vertex 0 red, 1 green, 2 blue at pixels (64,64), (192,64), (128,192) of a 256x256 target),
// gate 5's ray query (each ray's nearest triangle: distance, barycentrics, primitive id, or -1 for a miss).
kernel void vadd(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]], device float *c [[buffer(2)]],
                 uint i [[thread_position_in_grid]]) {
	c[i] = a[i] + b[i];
}

kernel void scale(device float *o [[buffer(0)]], constant float &k [[buffer(1)]], uint i [[thread_position_in_grid]]) {
	o[i] *= k;
}

#include <metal_stdlib>
using namespace metal;

struct TriOut {
	float4 position [[position]];
	float4 colour;
};

vertex TriOut tri_v(uint vid [[vertex_id]], device const float2 *pos [[buffer(0)]], device const float4 *colour [[buffer(1)]]) {
	TriOut o;
	o.position = float4(pos[vid], 0, 1);
	o.colour = colour[vid];
	return o;
}

fragment float4 tri_f(TriOut in [[stage_in]]) {
	return in.colour;
}

kernel void rays(raytracing::primitive_acceleration_structure accel [[buffer(0)]], device const float4 *origins [[buffer(1)]],
                 device const float4 *directions [[buffer(2)]], device float4 *out [[buffer(3)]], uint i [[thread_position_in_grid]]) {
	raytracing::ray r(origins[i].xyz, directions[i].xyz, 0.0f, 100.0f);
	raytracing::intersector<raytracing::triangle_data> isect;
	raytracing::intersection_result<raytracing::triangle_data> hit = isect.intersect(r, accel);
	out[i] = hit.type == raytracing::intersection_type::triangle ? float4(hit.distance, hit.triangle_barycentric_coord, float(hit.primitive_id))
	                                                              : float4(-1);
}

// mtlprobe native, instances: as MetalQuake's shadow rays (intersector<instancing>, no triangle data): {distance, instance, primitive, 0}.
kernel void rays_instanced(raytracing::instance_acceleration_structure accel [[buffer(0)]], device const float4 *origins [[buffer(1)]],
                           device const float4 *directions [[buffer(2)]], device float4 *out [[buffer(3)]], uint i [[thread_position_in_grid]]) {
	raytracing::ray r(origins[i].xyz, directions[i].xyz, 0.0f, 100.0f);
	raytracing::intersector<raytracing::instancing> isect;
	isect.assume_geometry_type(raytracing::geometry_type::triangle);
	auto hit = isect.intersect(r, accel);
	out[i] = hit.type != raytracing::intersection_type::none ? float4(hit.distance, float(hit.instance_id), float(hit.primitive_id), 0) : float4(-1);
}

// mtlprobe nativeargbuf: a texture, a sampler and the output buffer reached through a tier-2 argument buffer (MoltenVK's descriptor sets),
// which the app fills with their gpuResourceID / gpuAddress: out[i] = texel i of the 4x1 texture, read and sampled.
struct ArgProbe {
	texture2d<float> tex [[id(0)]];
	sampler smp [[id(1)]];
	device float4 *out [[id(2)]];
};

kernel void argbuf(constant ArgProbe &a [[buffer(0)]], uint i [[thread_position_in_grid]]) {
	a.out[i] = a.tex.read(uint2(i, 0));
	a.out[i + 4] = a.tex.sample(a.smp, float2((i + 0.5f) / 4.0f, 0.5f), level(0));
}

// mtlprobe native, bindless: a triangle over the 4x1 target whose fragment reads texture idx[x] of 128 (MetalQuake's static world binds 96 and
// 32 in one draw), so textures in all three of the table's ranges and its texture handles.
vertex float4 bindless_v(uint vid [[vertex_id]]) {
	return float4(float2((vid << 1) & 2, vid & 2) * 2 - 1, 0, 1);
}

fragment float4 bindless_f(float4 pos [[position]], array<texture2d<float>, 128> t [[texture(0)]], constant uint4 &idx [[buffer(0)]]) {
	return t[idx[min(uint(pos.x), 3u)]].read(uint2(0));
}

struct MipPlaneVertex {
	float4 position [[position]];
	float2 uv;
};

vertex MipPlaneVertex mip_plane_v(uint i [[vertex_id]], device const float2 *positions [[buffer(0)]]) {
	float2 p = positions[i];
	return {float4(p, 0, 1), p * 0.5 + 0.5};
}

fragment float4 mip_plane_f(MipPlaneVertex in [[stage_in]], texture2d<float> y [[texture(3)]],
                            texture2d<float> uv [[texture(5)]], sampler s [[sampler(0)]]) {
	bool root = y.get_width() == 3840 && y.get_height() == 2160 && y.get_num_mip_levels() == 12 && uv.get_num_mip_levels() == 11;
	bool view = y.get_width() == 960 && y.get_height() == 540 && y.get_num_mip_levels() == 2 && uv.get_num_mip_levels() == 2;
	return float4(y.sample(s, in.uv).r, uv.sample(s, in.uv).rg, root ? 1.0 : view ? 0.5 : 0.0);
}
