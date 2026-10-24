#version 450

layout(push_constant) uniform CursorPC {
    vec2  cursor_pos;
    vec2  blob_half;
    float corner_radius;
    float shadow_spread;
    float shadow_alpha;
    float fill_alpha;
    vec2  screen_size;
} pc;

layout(location = 0) in  vec2 frag_offset;
layout(location = 0) out vec4 out_color;

float roundedBoxSDF(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    vec2  b = pc.blob_half;
    float r = pc.corner_radius;

    // Drop shadow: slightly offset downward/right, exponential falloff
    const vec2 shadow_offset = vec2(2.0, 3.0);
    float sd_shad = roundedBoxSDF(frag_offset - shadow_offset, b, r);
    float shad_a  = exp(-max(sd_shad, 0.0) / max(pc.shadow_spread, 1.0))
                    * pc.shadow_alpha;

    // Blob fill: crisp AA edge
    float sd_fill = roundedBoxSDF(frag_offset, b, r);
    float fill_a  = (1.0 - smoothstep(-1.0, 1.0, sd_fill)) * pc.fill_alpha;

    // Composite fill (white) over shadow (dark) in shader
    float total_a = shad_a + fill_a * (1.0 - shad_a);
    if (total_a < 0.004) discard;

    float fill_rgb = fill_a / max(total_a, 0.001);
    // Slightly warm white (0.97, 0.97, 0.97) to appear softer
    out_color = vec4(fill_rgb * 0.97, fill_rgb * 0.97, fill_rgb * 0.97, total_a);
}
