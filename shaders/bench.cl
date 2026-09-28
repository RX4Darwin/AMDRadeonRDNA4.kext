// bench.cl — kernels for rdna4-run's LDS test and benchmarks, compiled by clang
// for gfx1201 into one code object (tools/build-shaders.sh ->
// src/bench_codeobj.h), so they also exercise loading a multi-kernel file.
//
// Built with -nogpulib: ids come from the AMDGPU builtins, and every kernel
// fixes its work-group size (reqd_work_group_size).

#define WG_ID(d)  __builtin_amdgcn_workgroup_id_##d()
#define LID_X     __builtin_amdgcn_workitem_id_x()
#define LID_Y     __builtin_amdgcn_workitem_id_y()
// OpenCL's barrier(CLK_LOCAL_MEM_FENCE), without the device library.
#define BARRIER() do { __builtin_amdgcn_fence(__ATOMIC_RELEASE, "workgroup"); \
                       __builtin_amdgcn_s_barrier(); \
                       __builtin_amdgcn_fence(__ATOMIC_ACQUIRE, "workgroup"); } while (0)

// LDS and a work-group barrier: each group of 64 stores its a[] slice in
// LDS, waits for the whole group, then reads it back reversed.
//   c[g*64 + i] = a[g*64 + 63 - i] + b[g*64 + i]
__kernel __attribute__((reqd_work_group_size(64, 1, 1)))
void lds_reverse(__global const uint *a, __global const uint *b, __global uint *c)
{
	__local uint tile[64];
	uint lid = LID_X, gid = WG_ID(x) * 64u + lid;
	tile[lid] = a[gid];
	BARRIER();
	c[gid] = tile[63u - lid] + b[gid];
}

// Memory bandwidth: 16 bytes per work-item, read once and written once.
__kernel __attribute__((reqd_work_group_size(256, 1, 1)))
void copy(__global const uint4 *src, __global uint4 *dst)
{
	uint gid = WG_ID(x) * 256u + LID_X;
	dst[gid] = src[gid];
}

// C = A x B, n x n row-major floats, n a multiple of 64.
// A 16x16 work-group computes a 64x64 tile of C, 4x4 outputs per work-item,
// staging 16-wide slices of A and B through LDS.
#define TS 64       // tile of C per work-group
#define TK 16       // depth of each LDS slice
#define WPT 4       // outputs per work-item, per dimension

__kernel __attribute__((reqd_work_group_size(16, 16, 1)))
void sgemm(__global const float *A, __global const float *B, __global float *C, uint n)
{
	__local float As[TK][TS + 1];   // As[k][row], padded against bank conflicts
	__local float Bs[TK][TS + 1];   // Bs[k][col]
	const uint tx = LID_X, ty = LID_Y;
	const uint row0 = WG_ID(y) * TS, col0 = WG_ID(x) * TS;
	const uint tid = ty * 16u + tx;
	float acc[WPT][WPT];
	for (int i = 0; i < WPT; i++)
		for (int j = 0; j < WPT; j++)
			acc[i][j] = 0.0f;

	for (uint k0 = 0; k0 < n; k0 += TK) {
		// 256 work-items load a 64x16 slice of A and a 16x64 slice of B:
		// four elements each.
		for (uint e = 0; e < 4; e++) {
			uint idx = tid + e * 256u;
			uint r = idx / TK, k = idx % TK;          // A: 64 rows x 16 k
			As[k][r] = A[(row0 + r) * n + k0 + k];
			uint kb = idx / TS, cb = idx % TS;        // B: 16 k x 64 cols
			Bs[kb][cb] = B[(k0 + kb) * n + col0 + cb];
		}
		BARRIER();
		for (uint k = 0; k < TK; k++) {
			float a[WPT], b[WPT];
			for (int i = 0; i < WPT; i++)
				a[i] = As[k][ty + 16u * i];
			for (int j = 0; j < WPT; j++)
				b[j] = Bs[k][tx + 16u * j];
			for (int i = 0; i < WPT; i++)
				for (int j = 0; j < WPT; j++)
					acc[i][j] = __builtin_fmaf(a[i], b[j], acc[i][j]);
		}
		BARRIER();
	}
	for (int i = 0; i < WPT; i++)
		for (int j = 0; j < WPT; j++)
			C[(row0 + ty + 16u * i) * n + col0 + tx + 16u * j] = acc[i][j];
}

// The matrix units: V_WMMA_F32_16X16X16_F16 / _BF16, one 16x16x16 product
// per wave per instruction, 16-bit inputs and FP32 accumulation. On gfx12
// lane l of a wave32 holds, as 8 16-bit values, row l % 16 of A
// (k = (l / 16) * 8 + 0..7) and column l % 16 of B (same k); as 8 floats,
// rows (l / 16) * 8 + 0..7 of column l % 16 of C.
#pragma OPENCL EXTENSION cl_khr_fp16 : enable
typedef half  half8  __attribute__((ext_vector_type(8)));
typedef float float8 __attribute__((ext_vector_type(8)));

static inline __attribute__((always_inline))
float8 mma(ushort8 a, ushort8 b, float8 c, bool bf16)
{
	return bf16 ? __builtin_amdgcn_wmma_f32_16x16x16_bf16_w32_gfx12(
	                  __builtin_astype(a, short8), __builtin_astype(b, short8), c)
	            : __builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(
	                  __builtin_astype(a, half8), __builtin_astype(b, half8), c);
}

// One wave, one 16x16x16 FP16 product: the layout check. Lane l gets
// fragment l of a and b and writes fragment l of c.
__kernel __attribute__((reqd_work_group_size(32, 1, 1)))
void wmma16(__global const ushort8 *a, __global const ushort8 *b, __global float8 *c)
{
	uint l = LID_X;
	c[l] = mma(a[l], b[l], c[l], false);
}

// C = A x Bt^T: A is n x n row-major 16-bit values, Bt is B transposed
// (n x n, row j = column j of B: the layout of a weight matrix), C is n x n
// floats, n a multiple of 128. 8 waves compute a 128x128 tile of C, 64x32
// each (4x2 WMMA tiles), staging 32-deep slices of A and Bt through LDS
// with k contiguous, so every fragment is one 16-byte LDS read. The next
// slice is fetched into registers while the current one is multiplied.
#define HT  128     // tile of C per work-group
#define HK  32      // depth of each LDS slice
#define HKP 40      // LDS row pitch in 16-bit values: 80 bytes, 16-byte aligned

static inline __attribute__((always_inline))
void gemm16(__global const ushort *A, __global const ushort *Bt, __global float *C, uint n,
            __local ushort *As, __local ushort *Bs, bool bf16)
{
	const uint tid = LID_X, lane = tid % 32u, wave = tid / 32u;
	const uint row0 = WG_ID(y) * HT, col0 = WG_ID(x) * HT;
	const uint wr = (wave / 4u) * 64u, wc = (wave % 4u) * 32u;   // the wave's 64x32
	const uint fr = lane % 16u, fk = (lane / 16u) * 8u;           // its fragment slot
	// Each work-item moves two 16-byte pieces of A's slice and two of Bt's:
	// rows r[e], k offset kq, of the 128 x 32 slices.
	const uint kq = (tid % 4u) * 8u, r[2] = { tid / 4u, tid / 4u + 64u };
	float8 acc[4][2];
	ushort8 pa[2], pb[2];
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 2; j++)
			acc[i][j] = (float8)(0.0f);
	for (int e = 0; e < 2; e++) {
		pa[e] = *(__global const ushort8 *)&A[(row0 + r[e]) * n + kq];
		pb[e] = *(__global const ushort8 *)&Bt[(col0 + r[e]) * n + kq];
	}

	for (uint k0 = 0; k0 < n; k0 += HK) {
		BARRIER();                              // the last slice is used up
		for (int e = 0; e < 2; e++) {
			*(__local ushort8 *)&As[r[e] * HKP + kq] = pa[e];
			*(__local ushort8 *)&Bs[r[e] * HKP + kq] = pb[e];
		}
		BARRIER();
		if (k0 + HK < n) {
			for (int e = 0; e < 2; e++) {
				pa[e] = *(__global const ushort8 *)&A[(row0 + r[e]) * n + k0 + HK + kq];
				pb[e] = *(__global const ushort8 *)&Bt[(col0 + r[e]) * n + k0 + HK + kq];
			}
		}
		for (uint kk = 0; kk < HK; kk += 16u) {
			ushort8 a[4], b[2];
			for (int i = 0; i < 4; i++)
				a[i] = *(__local const ushort8 *)&As[(wr + 16u * i + fr) * HKP + kk + fk];
			for (int j = 0; j < 2; j++)
				b[j] = *(__local const ushort8 *)&Bs[(wc + 16u * j + fr) * HKP + kk + fk];
			for (int i = 0; i < 4; i++)
				for (int j = 0; j < 2; j++)
					acc[i][j] = mma(a[i], b[j], acc[i][j], bf16);
		}
	}
	// Element e of acc[i][j] is row fk + e, column fr of that 16x16 tile.
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 2; j++)
			for (int e = 0; e < 8; e++)
				C[(row0 + wr + 16u * i + fk + e) * n + col0 + wc + 16u * j + fr] = acc[i][j][e];
}

// FP16 inputs (IEEE half).
__kernel __attribute__((reqd_work_group_size(256, 1, 1)))
void hgemm(__global const ushort *A, __global const ushort *Bt, __global float *C, uint n)
{
	__local ushort As[HT * HKP], Bs[HT * HKP];
	gemm16(A, Bt, C, n, As, Bs, false);
}

// BF16 inputs (the top half of a float).
__kernel __attribute__((reqd_work_group_size(256, 1, 1)))
void bf16gemm(__global const ushort *A, __global const ushort *Bt, __global float *C, uint n)
{
	__local ushort As[HT * HKP], Bs[HT * HKP];
	gemm16(A, Bt, C, n, As, Bs, true);
}
