// The bundle's own functions (Contents/Resources/rdna4.metallib, build.sh).
#include <metal_stdlib>
using namespace metal;

// A render pipeline's fragment stage when it has none (depth-only passes: MoltenVK's for vkQuake): writes nothing.
fragment void rdna4_no_fragment() {}
