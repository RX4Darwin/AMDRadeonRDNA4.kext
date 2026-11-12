#version 450
// The half of the picture above its falling diagonal: pixel (8,8) is inside, pixel (56,56) is not.
void main() {
	const vec2 corner[3] = vec2[3](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0));
	gl_Position = vec4(corner[gl_VertexIndex], 0.0, 1.0);
}
