#version 450

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform BlurData {
    vec2 dir;
    float resolution;
} pc;

void main() {
    vec2 uv = inTexCoord;
    float blur = 1.0 / pc.resolution;
    
    vec4 color = vec4(0.0);
    float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    
    color += texture(texSampler, uv) * weights[0];
    for(int i = 1; i < 5; ++i) {
        color += texture(texSampler, uv + vec2(blur * i, 0.0)) * weights[i];
        color += texture(texSampler, uv - vec2(blur * i, 0.0)) * weights[i];
    }
    
    outColor = color;
}
