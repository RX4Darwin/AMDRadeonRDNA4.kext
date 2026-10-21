// mtlvadd.cl — the precompiled stand-in for a Metal compute function until the AIR compiler exists (M2 of docs/metal-phase-plan.md: a table of
// gfx1201 kernels keyed by the Metal function). Matches tools/metal/m1/mtlprobe.m's kernel:
//     kernel void vadd(device const float *a, device const float *b, device float *c, uint i [[thread_position_in_grid]]) { c[i] = a[i] + b[i]; }
// The fake paravirtual GPU's host (pvgpu.cpp) runs it when a Metal app dispatches that function. Threads past `n` do nothing.
//
// Built like vadd.cl (tools/gen/build-shaders.sh, -nogpulib), with a fixed work-group size of 64.

__kernel __attribute__((reqd_work_group_size(64, 1, 1)))
void mtlvadd(__global const float *a, __global const float *b, __global float *c, uint n)
{
	uint i = __builtin_amdgcn_workgroup_id_x() * 64u + __builtin_amdgcn_workitem_id_x();
	if (i < n)
		c[i] = a[i] + b[i];
}
