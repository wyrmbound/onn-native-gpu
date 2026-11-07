#version 450
// Big triangle: base near the top of the image (Vulkan NDC y=-1 is top), apex near the bottom.
void main() {
    const vec2 p[3] = vec2[3](vec2(-0.9, -0.9), vec2(0.9, -0.9), vec2(0.0, 0.9));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
