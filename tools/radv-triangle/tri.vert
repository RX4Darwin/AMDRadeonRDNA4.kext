#version 450
// VertexID 0 -> (-0.5,-0.5), 1 -> (0.5,-0.5), 2 -> (0,0.5), z 0, w 1 (shaders/ngg.s).
void main() {
	float x = gl_VertexIndex == 1 ? 0.5 : 0.0;
	x = gl_VertexIndex == 0 ? -0.5 : x;
	float y = gl_VertexIndex == 2 ? 0.5 : -0.5;
	gl_Position = vec4(x, y, 0.0, 1.0);
}
