// vadd.cl — an OpenCL C kernel compiled by clang for gfx1201 into a full
// AMDGPU code object (tools/build-shaders.sh -> src/vadd_codeobj.h), to
// exercise the kext's code-object loader: c[i] = a[i] + 3 * b[i].
//
// Built with -nogpulib (no device library), so the global id comes from the
// AMDGPU builtins with a fixed work-group size of 64.

__kernel __attribute__((reqd_work_group_size(64, 1, 1)))
void vadd(__global const uint *a, __global const uint *b, __global uint *c)
{
	uint i = __builtin_amdgcn_workgroup_id_x() * 64u + __builtin_amdgcn_workitem_id_x();
	c[i] = a[i] + b[i] * 3u;
}
