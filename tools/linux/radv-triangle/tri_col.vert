#version 450
// G4: the G3 triangle (positions as tri.vert) with a per-vertex colour, vertex 0 red, 1 green, 2 blue.
layout(location = 0) out vec3 col;
void main() {
	float x = gl_VertexIndex == 1 ? 0.5 : 0.0;
	x = gl_VertexIndex == 0 ? -0.5 : x;
	float y = gl_VertexIndex == 2 ? 0.5 : -0.5;
	gl_Position = vec4(x, y, 0.0, 1.0);
	col = vec3(gl_VertexIndex == 0 ? 1.0 : 0.0, gl_VertexIndex == 1 ? 1.0 : 0.0, gl_VertexIndex == 2 ? 1.0 : 0.0);
}
