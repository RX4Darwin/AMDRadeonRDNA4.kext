// scale.cl — an OpenCL C kernel compiled by clang for gfx1201 into a full
// AMDGPU code object (tools/gen/build-shaders.sh -> src/scale_codeobj.h): a
// frame of macOS's scaled modes (5120x2880 for "looks like 2560x1440" on a
// 3840x2160 monitor) scaled down into the head's scanout, each pixel the
// average of the source pixels it covers (area filter, any ratio up to 2), or
// a smaller one (an HDR twin's 2560x1440 on its 4K base) scaled up with a
// Catmull-Rom bicubic filter: the area filter made each source pixel one or
// two pixels wide in turn at 1.5x, text and edges blocky (card, 2026-11-28).
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

// Catmull-Rom weights for the four taps around a sample `t` (0..1) past the second.
static float4 catmullRom(float t)
{
	const float t2 = t * t, t3 = t2 * t;

	return (float4)(-0.5f * t3 + t2 - 0.5f * t, 1.5f * t3 - 2.5f * t2 + 1.0f, -1.5f * t3 + 2.0f * t2 + 0.5f * t, 0.5f * t3 - 0.5f * t2);
}

static float4 bicubic(__global const uint *src, uint srcPitch, uint srcW, uint srcH, float fx, float fy, uint tenBit)
{
	const float bx = __builtin_floorf(fx), by = __builtin_floorf(fy);
	const float4 wx = catmullRom(fx - bx), wy = catmullRom(fy - by);
	const float wxs[4] = { wx.x, wx.y, wx.z, wx.w }, wys[4] = { wy.x, wy.y, wy.z, wy.w };
	float4 sum = (float4)(0.0f);

	for (int j = 0; j < 4; j++) {
		const int sy = __builtin_elementwise_min(__builtin_elementwise_max((int)by - 1 + j, 0), (int)srcH - 1);
		__global const uint *row = src + (ulong)sy * srcPitch;
		float4 line = (float4)(0.0f);

		for (int i = 0; i < 4; i++)
			line += wxs[i] * unpack(row[__builtin_elementwise_min(__builtin_elementwise_max((int)bx - 1 + i, 0), (int)srcW - 1)], tenBit);
		sum += wys[j] * line;
	}
	return sum;
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

	if (srcW < dstW && srcH < dstH) {   // scaled up: bicubic at the pixel's centre, kept within the channels' range
		const float4 hi = tenBit ? (float4)(1023.0f, 1023.0f, 1023.0f, 3.0f) : (float4)(255.0f);
		const float4 c = bicubic(src, srcPitch, srcW, srcH, (x + 0.5f) * sx - 0.5f, (y + 0.5f) * sy - 0.5f, tenBit);

		const float4 k = (float4)(__builtin_fminf(__builtin_fmaxf(c.x, 0.0f), hi.x), __builtin_fminf(__builtin_fmaxf(c.y, 0.0f), hi.y),
		                          __builtin_fminf(__builtin_fmaxf(c.z, 0.0f), hi.z), __builtin_fminf(__builtin_fmaxf(c.w, 0.0f), hi.w));

		dst[(ulong)y * dstPitch + x] = pack(k, tenBit);
		return;
	}

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
