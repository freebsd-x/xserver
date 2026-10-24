#version 450

layout(location = 0) in  vec2 local_pos;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PC {
    vec2  pos;
    vec2  size;
    vec2  screen_size;
    float radius;
    float pad;
    vec4  color;
} pc;

// Signed distance field for a rounded rectangle centered at origin,
// half-extents `b`, corner radius `r`.  Negative inside, positive outside.
float roundedBoxSDF(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    vec2  hsize = pc.size * 0.5;
    float d     = roundedBoxSDF(local_pos - hsize, hsize, pc.radius);
    float a     = 1.0 - smoothstep(-0.5, 0.5, d);
    out_color  = vec4(pc.color.rgb, pc.color.a * a);
}
