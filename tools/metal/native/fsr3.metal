// fsr3.metal: the clears of the MetalFX temporal scaler (RDNA4FXTemporal.mm) between FSR 3.1's passes, in the bundle's fsr3.metallib
// (tools/gen/build-fsr3.sh): AMD's backends clear these resources with the API's clears.
#include <metal_stdlib>
using namespace metal;

kernel void fsr3_clear_texture(texture2d<float, access::write> t [[texture(0)]], constant float4 &value [[buffer(0)]],
                               uint2 p [[thread_position_in_grid]])
{
	if (p.x < t.get_width() && p.y < t.get_height())
		t.write(value, p);
}

kernel void fsr3_clear_words(device uint *words [[buffer(0)]], constant uint2 &value [[buffer(1)]], uint i [[thread_position_in_grid]])
{
	if (i < value.y)   // (value.x into words 0 .. value.y - 1)
		words[i] = value.x;
}
