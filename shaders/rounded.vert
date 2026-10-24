#version 450

// Push constants (48 bytes, matches pipeline_layout_flat_):
//   vec2  pos         — top-left in screen pixels
//   vec2  size        — width × height in pixels
//   vec2  screen_size — output resolution
//   float radius      — corner radius in pixels
//   float pad
//   vec4  color       — RGBA
layout(push_constant) uniform PC {
    vec2  pos;
    vec2  size;
    vec2  screen_size;
    float radius;
    float pad;
    vec4  color;
} pc;

layout(location = 0) out vec2 local_pos; // pixel coords within the quad

void main() {
    const vec2 corners[4] = vec2[](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));
    vec2 local  = corners[gl_VertexIndex] * pc.size;
    local_pos   = local;
    vec2 pixel  = pc.pos + local;
    gl_Position = vec4((pixel / pc.screen_size) * 2.0 - 1.0, 0.0, 1.0);
}
