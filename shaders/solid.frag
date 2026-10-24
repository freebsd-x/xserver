#version 450

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform SolidData {
    vec2 pos;
    vec2 size;
    vec2 screen_size;
    vec4 color;
} pc;

void main() {
    outColor = pc.color;
}
