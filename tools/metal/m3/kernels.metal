// The Metal compute kernels the fake paravirtual GPU's host runs on the card (M3, docs/m1-stream.md s.5b), translated by
// tools/metal/m3/build.sh into src/mtlkernels.h. The host picks one by the function's name, so each must match, character for
// character in its signature, the kernel tools/metal/m1/mtlprobe.m compiles on the guest.
kernel void vadd(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]],
                 device float *c [[buffer(2)]], uint i [[thread_position_in_grid]]) { c[i] = a[i] + b[i]; }

kernel void grid2d(device uint *out [[buffer(0)]], uint2 p [[thread_position_in_grid]], uint2 n [[threads_per_grid]]) {
	out[p.y * n.x + p.x] = p.x + 1000 * p.y;
}
