// The bundle's own functions (Contents/Resources/rdna4.metallib, build.sh).
#include <metal_stdlib>
using namespace metal;

// A render pipeline's fragment stage when it has none (depth-only passes: MoltenVK's for vkQuake): writes nothing.
fragment void rdna4_no_fragment() {}

// A colour load action's clear as a draw (RDNA4MTLRenderEncoder clearByDraw): a triangle over the whole target, each fragment the clear
// colour (buffer 0). For the colours no 32-bit fill makes (RGBA16Float's, MetalQuake's HDR targets) and targets in the card's memory only.
struct rdna4_clear_position { float4 position [[position]]; };

vertex rdna4_clear_position rdna4_clear_vertex(uint vid [[vertex_id]])
{
	const float2 uv = float2((vid << 1) & 2, vid & 2);
	return { float4(uv * 2.0f - 1.0f, 0.0f, 1.0f) };
}

fragment float4 rdna4_clear_fragment0(constant float4 &colour [[buffer(0)]]) { return colour; }
struct rdna4_clear1 { float4 c [[color(1)]]; };
fragment rdna4_clear1 rdna4_clear_fragment1(constant float4 &colour [[buffer(0)]]) { return { colour }; }
struct rdna4_clear2 { float4 c [[color(2)]]; };
fragment rdna4_clear2 rdna4_clear_fragment2(constant float4 &colour [[buffer(0)]]) { return { colour }; }
struct rdna4_clear3 { float4 c [[color(3)]]; };
fragment rdna4_clear3 rdna4_clear_fragment3(constant float4 &colour [[buffer(0)]]) { return { colour }; }
