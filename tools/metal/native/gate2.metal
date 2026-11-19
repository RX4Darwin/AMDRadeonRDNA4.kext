// The native-Metal spike's gate 2 kernels (mtlprobe native <gate2.metallib>, docs/native-metal-spike.md).
kernel void vadd(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]], device float *c [[buffer(2)]],
                 uint i [[thread_position_in_grid]]) {
	c[i] = a[i] + b[i];
}

kernel void scale(device float *o [[buffer(0)]], constant float &k [[buffer(1)]], uint i [[thread_position_in_grid]]) {
	o[i] *= k;
}
