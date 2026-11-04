#version 450

// Every caller pushes 10 floats, the colour at offset 24. Four floats, not a vec4:
// a vec4 is aligned to 16 bytes, which would read it from offset 32.
layout(push_constant) uniform SolidData {
    vec2 pos;
    vec2 size;
    vec2 screen_size;
    float r, g, b, a;
} pc;

void main() {
    vec2 uv[4] = vec2[](
        vec2(0.0, 0.0),
        vec2(1.0, 0.0),
        vec2(0.0, 1.0),
        vec2(1.0, 1.0)
    );
    vec2 pixel_pos = pc.pos + uv[gl_VertexIndex] * pc.size;
    vec2 ndc = (pixel_pos / pc.screen_size) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
