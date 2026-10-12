#version 450
// G4: the interpolated vertex colour (smooth, perspective-correct; w = 1 so it is plain linear), alpha 1.
layout(location = 0) in vec3 col;
layout(location = 0) out vec4 color;
void main() { color = vec4(col, 1.0); }
