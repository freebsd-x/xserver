#version 450

layout(push_constant) uniform WindowData {
    vec2 pos;
    vec2 size;
    vec2 screen_size;
    float scale;
    float opacity;
    float scene_offset;
} pc;

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out float outOpacity;

void main() {
    vec2 base_pos[4] = vec2[](
        vec2(0.0, 0.0),
        vec2(1.0, 0.0),
        vec2(0.0, 1.0),
        vec2(1.0, 1.0)
    );
    
    vec2 uv = base_pos[gl_VertexIndex];
    outTexCoord = uv;
    outOpacity = pc.opacity;
    
    vec2 pixel_pos = pc.pos + (uv * pc.size);
    pixel_pos.x += pc.scene_offset;
    
    vec2 ndc_pos = (pixel_pos / pc.screen_size) * 2.0 - 1.0;
    gl_Position = vec4(ndc_pos, 0.0, 1.0);
}
