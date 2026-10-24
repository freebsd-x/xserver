#version 450

layout(location = 0) in vec2 inPosition;

layout(push_constant) uniform PushConstants {
    vec2 offset;
    vec2 screen_size;
    vec4 color;
} pc;

void main() {
    vec2 pos = inPosition + pc.offset;
    vec2 ndc = (pos / pc.screen_size) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
