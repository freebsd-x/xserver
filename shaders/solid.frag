#version 450

layout(location = 0) out vec4 outColor;

// Every caller pushes 10 floats, the colour at offset 24. Four floats, not a vec4:
// a vec4 is aligned to 16 bytes, which would read it from offset 32.
layout(push_constant) uniform SolidData {
    vec2 pos;
    vec2 size;
    vec2 screen_size;
    float r, g, b, a;
} pc;

void main() {
    outColor = vec4(pc.r, pc.g, pc.b, pc.a);
}
