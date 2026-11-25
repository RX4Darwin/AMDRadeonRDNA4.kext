// fsr1.cl — AMD FidelityFX Super Resolution 1 (EASU upscaling, RCAS sharpening), ported by hand to OpenCL C from the 32-bit
// float path of lib/FidelityFX-FSR/ffx-fsr/ffx_fsr1.h (FsrEasuF, FsrEasuSetF, FsrEasuTapF, FsrRcasF; FSR v1.0.2) for the
// driver's upscaling (docs/fsr1-upscaling-plan.md). Compiled by clang for gfx1201 with -nogpulib (tools/gen/build-shaders.sh),
// and as C on the Mac by test/fsr1-test.c, which checks the same pixel functions.
//
// Buffers, not textures: EASU's four gathers are twelve loads, edges clamped. `fmt` says what a pixel is: 0 four 8-bit channels
// (as scale.cl's), 1 three 10-bit ones and a 2-bit alpha, 2 four half floats (RGBA16Float, 64 bits); plus 4 for HDR colour, run
// through AMD's reversible tonemap (FsrSrtm / FsrSrtmInv) so EASU and RCAS see 0-1 values. The constants FsrEasuCon and FsrRcasCon
// make are computed here from the integer sizes and an integer sharpness, so the caller does no floating point. EASU into a buffer
// of the output size, then RCAS from it into the target; alpha is the nearest source pixel's (EASU) and passed through (RCAS).
// Pitches are in pixels.
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software
// is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

// Every function inlined: a call would need a stack (scratch memory), which the kernels are loaded without.
#define FSR_FN static inline __attribute__((always_inline))
#ifdef __OPENCL_C_VERSION__
#define FSR_GLOBAL __global
#else   // (C on the host: test/fsr1-test.c)
#include <stdint.h>
typedef uint32_t uint;
typedef uint64_t ulong;
#define FSR_GLOBAL
#endif

// A pixel's channels in 0-1: x, y, z the three colour channels in memory order (luma weighs the first and third alike, so BGRA
// and RGBA are the same to it), w alpha.
typedef struct { float x, y, z, w; } FsrPixel;

FSR_FN float fsrMin(float a, float b) { return __builtin_fminf(a, b); }
FSR_FN float fsrMax(float a, float b) { return __builtin_fmaxf(a, b); }
FSR_FN float fsrSat(float a) { return fsrMin(fsrMax(a, 0.0f), 1.0f); }
FSR_FN float fsrAsFloat(uint u) { union { uint u; float f; } v; v.u = u; return v.f; }
FSR_FN uint fsrAsUint(float f) { union { uint u; float f; } v; v.f = f; return v.u; }
// ffx_a.h's approximations (APrxLoRcpF1, APrxMedRcpF1, APrxLoRsqF1)
FSR_FN float fsrRcpLo(float a) { return fsrAsFloat(0x7ef07ebbu - fsrAsUint(a)); }
FSR_FN float fsrRcpMed(float a) { const float b = fsrAsFloat(0x7ef19fffu - fsrAsUint(a)); return b * (-b * a + 2.0f); }
FSR_FN float fsrRsqLo(float a) { return fsrAsFloat(0x5f347d74u - (fsrAsUint(a) >> 1)); }

// IEEE half <-> float by their bits (no half type: the host check runs the same code)
FSR_FN float fsrHalf(uint h)
{
	const uint sign = (h & 0x8000u) << 16, e = h >> 10 & 0x1fu, m = h & 0x3ffu;
	if (!e)
		return fsrAsFloat(sign) + (sign ? -1.0f : 1.0f) * (float)m * (1.0f / 16777216.0f);   // zero or denormal: m * 2^-24
	return fsrAsFloat(sign | (e == 31 ? 0x7f800000u : (e + 112) << 23) | m << 13);
}

FSR_FN uint fsrToHalf(float f)
{
	const uint x = fsrAsUint(f), sign = x >> 16 & 0x8000u, m = x & 0x7fffffu;
	const int e = (int)(x >> 23 & 0xff) - 112;
	if ((x & 0x7fffffffu) > 0x7f800000u)
		return sign | 0x7e00u;   // NaN
	if (e >= 31)
		return sign | 0x7c00u;   // too large: infinity
	if (e <= 0) {                // a denormal half, or zero
		if (e < -10)
			return sign;
		const uint full = m | 0x800000u, shift = (uint)(14 - e);
		return sign | ((full >> shift) + (full >> (shift - 1) & 1u));
	}
	return (sign | (uint)e << 10 | m >> 13) + (m >> 12 & 1u);   // rounded half up (a carry moves into the exponent, as it should)
}

FSR_FN FsrPixel fsrLoad(const FSR_GLOBAL uint *src, uint pitch, uint w, uint h, int x, int y, uint fmt)
{
	x = x < 0 ? 0 : x >= (int)w ? (int)w - 1 : x;
	y = y < 0 ? 0 : y >= (int)h ? (int)h - 1 : y;
	const ulong at = (ulong)y * pitch + (uint)x;
	FsrPixel p;
	if ((fmt & 3) == 2) {
		const uint lo = src[2 * at], hi = src[2 * at + 1];
		p.x = fsrHalf(lo & 0xffffu), p.y = fsrHalf(lo >> 16), p.z = fsrHalf(hi & 0xffffu), p.w = fsrHalf(hi >> 16);
		if (fmt & 4) {   // FsrSrtm: c / (1 + max(c)), 0-1 for any c >= 0
			const float r = 1.0f / (1.0f + fsrMax(fsrMax(p.x, p.y), fsrMax(p.z, 0.0f)));
			p.x *= r, p.y *= r, p.z *= r;
		}
		return p;
	}
	const uint v = src[at];
	if (fmt & 1) {
		p.x = (v & 0x3ffu) * (1.0f / 1023.0f), p.y = (v >> 10 & 0x3ffu) * (1.0f / 1023.0f), p.z = (v >> 20 & 0x3ffu) * (1.0f / 1023.0f);
		p.w = (v >> 30) * (1.0f / 3.0f);
	} else {
		p.x = (v & 0xffu) * (1.0f / 255.0f), p.y = (v >> 8 & 0xffu) * (1.0f / 255.0f), p.z = (v >> 16 & 0xffu) * (1.0f / 255.0f);
		p.w = (v >> 24) * (1.0f / 255.0f);
	}
	return p;
}

// Pixel (x, y) of dst written as `fmt` says
FSR_FN void fsrStore(FSR_GLOBAL uint *dst, uint pitch, uint x, uint y, FsrPixel p, uint fmt)
{
	const ulong at = (ulong)y * pitch + x;
	if ((fmt & 3) == 2) {
		if (fmt & 4) {   // FsrSrtmInv: c / (1 - max(c))
			const float r = 1.0f / fsrMax(1.0f - fsrMax(fsrMax(p.x, p.y), fsrMax(p.z, 0.0f)), 1.0f / 65504.0f);
			p.x *= r, p.y *= r, p.z *= r;
		}
		dst[2 * at] = fsrToHalf(p.x) | fsrToHalf(p.y) << 16;
		dst[2 * at + 1] = fsrToHalf(p.z) | fsrToHalf(p.w) << 16;
		return;
	}
	const float s = fmt & 1 ? 1023.0f : 255.0f;
	const uint r = (uint)(fsrSat(p.x) * s + 0.5f), g = (uint)(fsrSat(p.y) * s + 0.5f), b = (uint)(fsrSat(p.z) * s + 0.5f);
	dst[at] = fmt & 1 ? r | g << 10 | b << 20 | (uint)(fsrSat(p.w) * 3.0f + 0.5f) << 30
	                  : r | g << 8 | b << 16 | (uint)(fsrSat(p.w) * 255.0f + 0.5f) << 24;
}

FSR_FN float fsrLuma(FsrPixel p) { return p.z * 0.5f + (p.x * 0.5f + p.y); }   // luma times 2

// FsrEasuSetF: one bilinear corner's share of the direction and length, from the '+' of luma around its centre c
FSR_FN void fsrEasuSet(float *dirX, float *dirY, float *len, float w, float lA, float lB, float lC, float lD, float lE)
{
	const float dc = lD - lC, cb = lC - lB;
	float lenX = fsrRcpLo(fsrMax(__builtin_fabsf(dc), __builtin_fabsf(cb)));
	const float dx = lD - lB;
	*dirX += dx * w;
	lenX = fsrSat(__builtin_fabsf(dx) * lenX);
	*len += lenX * lenX * w;
	const float ec = lE - lC, ca = lC - lA;
	float lenY = fsrRcpLo(fsrMax(__builtin_fabsf(ec), __builtin_fabsf(ca)));
	const float dy = lE - lA;
	*dirY += dy * w;
	lenY = fsrSat(__builtin_fabsf(dy) * lenY);
	*len += lenY * lenY * w;
}

// FsrEasuTapF: one tap's weight (an approximation of Lanczos 2 along the edge direction) into the sums
FSR_FN void fsrEasuTap(FsrPixel *sum, float *weight, float offX, float offY, float dirX, float dirY, float lenX, float lenY,
                       float lob, float clp, FsrPixel c)
{
	const float vx = (offX * dirX + offY * dirY) * lenX, vy = (offX * -dirY + offY * dirX) * lenY;
	const float d2 = fsrMin(vx * vx + vy * vy, clp);
	float wB = (2.0f / 5.0f) * d2 - 1.0f, wA = lob * d2 - 1.0f;
	wB *= wB, wA *= wA;
	wB = (25.0f / 16.0f) * wB - (25.0f / 16.0f - 1.0f);
	const float w = wB * wA;
	sum->x += c.x * w, sum->y += c.y * w, sum->z += c.z * w;
	*weight += w;
}

// v within the range of four values (EASU's dering: the four nearest pixels)
FSR_FN float fsrDering(float v, float a, float b, float c, float d)
{
	return fsrMin(fsrMax(fsrMax(a, b), fsrMax(c, d)), fsrMax(fsrMin(fsrMin(a, b), fsrMin(c, d)), v));
}

// FsrEasuF: output pixel (x, y) of a source srcW x srcH scaled to dstW x dstH
FSR_FN FsrPixel fsrEasuPixel(const FSR_GLOBAL uint *src, uint pitch, uint srcW, uint srcH, uint dstW, uint dstH, uint x, uint y, uint fmt)
{
	// its position in the source (FsrEasuCon's con0: (ip + 0.5) * in / out - 0.5), and the source pixel 'f' up and left of it
	float ppX = ((float)x + 0.5f) * (float)srcW / (float)dstW - 0.5f, ppY = ((float)y + 0.5f) * (float)srcH / (float)dstH - 0.5f;
	const float fX = __builtin_floorf(ppX), fY = __builtin_floorf(ppY);
	ppX -= fX, ppY -= fY;
	const int ix = (int)fX, iy = (int)fY;
	//    b c
	//  e f g h
	//  i j k l
	//    n o
#define FSR_TAP(dx, dy) fsrLoad(src, pitch, srcW, srcH, ix + (dx), iy + (dy), fmt)
	const FsrPixel b = FSR_TAP(0, -1), c = FSR_TAP(1, -1), e = FSR_TAP(-1, 0), f = FSR_TAP(0, 0), g = FSR_TAP(1, 0), h = FSR_TAP(2, 0);
	const FsrPixel i = FSR_TAP(-1, 1), j = FSR_TAP(0, 1), k = FSR_TAP(1, 1), l = FSR_TAP(2, 1), n = FSR_TAP(0, 2), o = FSR_TAP(1, 2);
#undef FSR_TAP
	const float bL = fsrLuma(b), cL = fsrLuma(c), eL = fsrLuma(e), fL = fsrLuma(f), gL = fsrLuma(g), hL = fsrLuma(h);
	const float iL = fsrLuma(i), jL = fsrLuma(j), kL = fsrLuma(k), lL = fsrLuma(l), nL = fsrLuma(n), oL = fsrLuma(o);
	float dirX = 0.0f, dirY = 0.0f, len = 0.0f;
	fsrEasuSet(&dirX, &dirY, &len, (1.0f - ppX) * (1.0f - ppY), bL, eL, fL, gL, jL);
	fsrEasuSet(&dirX, &dirY, &len, ppX * (1.0f - ppY), cL, fL, gL, hL, kL);
	fsrEasuSet(&dirX, &dirY, &len, (1.0f - ppX) * ppY, fL, iL, jL, kL, nL);
	fsrEasuSet(&dirX, &dirY, &len, ppX * ppY, gL, jL, kL, lL, oL);
	// the direction normalised (none near zero: along x), the length shaped
	const float dirR2 = dirX * dirX + dirY * dirY;
	const int zero = dirR2 < 1.0f / 32768.0f;
	const float dirR = zero ? 1.0f : fsrRsqLo(dirR2);
	dirX = (zero ? 1.0f : dirX) * dirR, dirY *= dirR;
	len *= 0.5f;
	len *= len;
	const float stretch = (dirX * dirX + dirY * dirY) * fsrRcpLo(fsrMax(__builtin_fabsf(dirX), __builtin_fabsf(dirY)));
	const float lenX = 1.0f + (stretch - 1.0f) * len, lenY = 1.0f - 0.5f * len;
	const float lob = 0.5f + ((1.0f / 4.0f - 0.04f) - 0.5f) * len, clp = fsrRcpLo(lob);
	FsrPixel sum = { 0.0f, 0.0f, 0.0f, 0.0f };
	float weight = 0.0f;
	fsrEasuTap(&sum, &weight, 0.0f - ppX, -1.0f - ppY, dirX, dirY, lenX, lenY, lob, clp, b);
	fsrEasuTap(&sum, &weight, 1.0f - ppX, -1.0f - ppY, dirX, dirY, lenX, lenY, lob, clp, c);
	fsrEasuTap(&sum, &weight, -1.0f - ppX, 1.0f - ppY, dirX, dirY, lenX, lenY, lob, clp, i);
	fsrEasuTap(&sum, &weight, 0.0f - ppX, 1.0f - ppY, dirX, dirY, lenX, lenY, lob, clp, j);
	fsrEasuTap(&sum, &weight, 0.0f - ppX, 0.0f - ppY, dirX, dirY, lenX, lenY, lob, clp, f);
	fsrEasuTap(&sum, &weight, -1.0f - ppX, 0.0f - ppY, dirX, dirY, lenX, lenY, lob, clp, e);
	fsrEasuTap(&sum, &weight, 1.0f - ppX, 1.0f - ppY, dirX, dirY, lenX, lenY, lob, clp, k);
	fsrEasuTap(&sum, &weight, 2.0f - ppX, 1.0f - ppY, dirX, dirY, lenX, lenY, lob, clp, l);
	fsrEasuTap(&sum, &weight, 2.0f - ppX, 0.0f - ppY, dirX, dirY, lenX, lenY, lob, clp, h);
	fsrEasuTap(&sum, &weight, 1.0f - ppX, 0.0f - ppY, dirX, dirY, lenX, lenY, lob, clp, g);
	fsrEasuTap(&sum, &weight, 1.0f - ppX, 2.0f - ppY, dirX, dirY, lenX, lenY, lob, clp, o);
	fsrEasuTap(&sum, &weight, 0.0f - ppX, 2.0f - ppY, dirX, dirY, lenX, lenY, lob, clp, n);
	// normalised, and deringed: within the four nearest pixels' range (f, g, j, k)
	const float r = 1.0f / weight;
	FsrPixel out;
	out.x = fsrDering(sum.x * r, f.x, g.x, j.x, k.x);
	out.y = fsrDering(sum.y * r, f.y, g.y, j.y, k.y);
	out.z = fsrDering(sum.z * r, f.z, g.z, j.z, k.z);
	out.w = (ppX < 0.5f ? (ppY < 0.5f ? f : j) : (ppY < 0.5f ? g : k)).w;   // the nearest pixel's alpha
	return out;
}

// FsrRcasF: pixel (x, y) of a w x h image sharpened; `sharpness` in thousandths of a stop (0 sharpest; AMD's samples use 200).
// ponytail: no FSR_RCAS_DENOISE (its noise detection is left out, as AMD's default).
FSR_FN float fsrRcasLobe(float b, float d, float e, float f, float h)
{
	const float mn4 = fsrMin(fsrMin(b, d), fsrMin(f, h)), mx4 = fsrMax(fsrMax(b, d), fsrMax(f, h));
	const float hitMin = fsrMin(mn4, e) * (1.0f / (4.0f * mx4));
	const float hitMax = (1.0f - fsrMax(mx4, e)) * (1.0f / (4.0f * mn4 - 4.0f));
	return fsrMax(-hitMin, hitMax);
}

FSR_FN FsrPixel fsrRcasPixel(const FSR_GLOBAL uint *src, uint pitch, uint w, uint h, uint x, uint y, uint sharpness, uint fmt)
{
	//    b
	//  d e f
	//    h
	const FsrPixel b = fsrLoad(src, pitch, w, h, (int)x, (int)y - 1, fmt), d = fsrLoad(src, pitch, w, h, (int)x - 1, (int)y, fmt);
	const FsrPixel e = fsrLoad(src, pitch, w, h, (int)x, (int)y, fmt), f = fsrLoad(src, pitch, w, h, (int)x + 1, (int)y, fmt);
	const FsrPixel hh = fsrLoad(src, pitch, w, h, (int)x, (int)y + 1, fmt);
	const float limit = 0.25f - 1.0f / 16.0f;   // FSR_RCAS_LIMIT
	const float lobe = fsrMax(-limit, fsrMin(fsrMax(fsrMax(fsrRcasLobe(b.x, d.x, e.x, f.x, hh.x), fsrRcasLobe(b.y, d.y, e.y, f.y, hh.y)),
	                                                fsrRcasLobe(b.z, d.z, e.z, f.z, hh.z)),
	                                         0.0f)) *
	                   __builtin_exp2f(-(float)sharpness / 1000.0f);
	const float r = fsrRcpMed(4.0f * lobe + 1.0f);
	FsrPixel out;
	out.x = (lobe * (b.x + d.x + hh.x + f.x) + e.x) * r;
	out.y = (lobe * (b.y + d.y + hh.y + f.y) + e.y) * r;
	out.z = (lobe * (b.z + d.z + hh.z + f.z) + e.z) * r;
	out.w = e.w;
	return out;
}

#ifdef __OPENCL_C_VERSION__
// One work-item per output pixel, a group of 64 along a row, a row per group in y (as scale.cl).
__kernel __attribute__((reqd_work_group_size(64, 1, 1)))
void easu(__global const uint *src, __global uint *dst, uint srcPitch, uint dstPitch, uint srcW, uint srcH, uint dstW, uint dstH,
          uint fmt)
{
	const uint x = __builtin_amdgcn_workgroup_id_x() * 64u + __builtin_amdgcn_workitem_id_x(), y = __builtin_amdgcn_workgroup_id_y();
	if (x < dstW && y < dstH)
		fsrStore(dst, dstPitch, x, y, fsrEasuPixel(src, srcPitch, srcW, srcH, dstW, dstH, x, y, fmt), fmt);
}

__kernel __attribute__((reqd_work_group_size(64, 1, 1)))
void rcas(__global const uint *src, __global uint *dst, uint srcPitch, uint dstPitch, uint w, uint h, uint sharpness, uint fmt)
{
	const uint x = __builtin_amdgcn_workgroup_id_x() * 64u + __builtin_amdgcn_workitem_id_x(), y = __builtin_amdgcn_workgroup_id_y();
	if (x < w && y < h)
		fsrStore(dst, dstPitch, x, y, fsrRcasPixel(src, srcPitch, w, h, x, y, sharpness, fmt), fmt);
}
#endif
