// scale.cl — an OpenCL C kernel compiled by clang for gfx1201 into a full
// AMDGPU code object (tools/gen/build-shaders.sh -> src/scale_codeobj.h): a
// frame of macOS's scaled modes (5120x2880 for "looks like 2560x1440" on a
// 3840x2160 monitor) scaled down into the head's scanout, each pixel the
// average of the source pixels it covers (area filter, any ratio up to 2).
//
// One work-item per destination pixel, a group of 64 along a row, a row per
// group in y. Pixels are 32 bits: four 8-bit channels, or with `tenBit` three
// 10-bit ones and a 2-bit alpha, averaged channel by channel either way.
// Built with -nogpulib (no device library): the AMDGPU builtins only.

static float4 unpack(uint v, uint tenBit)
{
	if (tenBit)
		return (float4)(v & 0x3ffu, v >> 10 & 0x3ffu, v >> 20 & 0x3ffu, v >> 30);

	return (float4)(v & 0xffu, v >> 8 & 0xffu, v >> 16 & 0xffu, v >> 24);
}

static uint pack(float4 c, uint tenBit)
{
	const uint4 u = (uint4)((uint)(c.x + 0.5f), (uint)(c.y + 0.5f), (uint)(c.z + 0.5f), (uint)(c.w + 0.5f));

	if (tenBit)
		return u.x | u.y << 10 | u.z << 20 | u.w << 30;

	return u.x | u.y << 8 | u.z << 16 | u.w << 24;
}

__kernel __attribute__((reqd_work_group_size(64, 1, 1)))
void scale(__global const uint *src, __global uint *dst, uint srcPitch, uint dstPitch, uint srcW, uint srcH, uint dstW,
           uint dstH, uint tenBit)
{
	const uint x = __builtin_amdgcn_workgroup_id_x() * 64u + __builtin_amdgcn_workitem_id_x();
	const uint y = __builtin_amdgcn_workgroup_id_y();

	if (x >= dstW || y >= dstH)
		return;

	const float sx = (float)srcW / (float)dstW, sy = (float)srcH / (float)dstH;
	const float x0 = x * sx, x1 = x0 + sx, y0 = y * sy, y1 = y0 + sy;
	float4 sum = (float4)(0.0f);
	float weight = 0.0f;

	for (uint j = (uint)y0; j < srcH && (float)j < y1; j++) {
		const float wy = __builtin_fminf(y1, j + 1.0f) - __builtin_fmaxf(y0, (float)j);
		__global const uint *row = src + (ulong)j * srcPitch;

		for (uint i = (uint)x0; i < srcW && (float)i < x1; i++) {
			const float w = wy * (__builtin_fminf(x1, i + 1.0f) - __builtin_fmaxf(x0, (float)i));

			sum += w * unpack(row[i], tenBit);
			weight += w;
		}
	}

	dst[(ulong)y * dstPitch + x] = pack(sum / weight, tenBit);
}
