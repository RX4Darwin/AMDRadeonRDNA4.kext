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
