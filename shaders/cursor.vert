#version 450

layout(push_constant) uniform CursorPC {
    vec2  cursor_pos;     // screen-space pixel center of blob
    vec2  blob_half;      // half-size of the animated blob (half-width, half-height)
    float corner_radius;  // animated corner radius
    float shadow_spread;  // shadow blur distance in pixels
    float shadow_alpha;   // shadow opacity
    float fill_alpha;     // fill opacity
    vec2  screen_size;    // physical output resolution
} pc;

layout(location = 0) out vec2 frag_offset;  // pixel offset from blob center

void main() {
    float pad = pc.shadow_spread + 10.0;
    vec2  half_ext = pc.blob_half + vec2(pad);

    const vec2 corners[4] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 1.0, -1.0),
        vec2(-1.0,  1.0),
        vec2( 1.0,  1.0)
    );

    vec2 c       = corners[gl_VertexIndex];
    frag_offset  = c * half_ext;
    vec2 pix_pos = pc.cursor_pos + frag_offset;
    vec2 ndc     = (pix_pos / pc.screen_size) * 2.0 - 1.0;
    gl_Position  = vec4(ndc, 0.0, 1.0);
}
