#version 450

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in float inOpacity;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

void main() {
    vec4 texColor = texture(texSampler, inTexCoord);

    // Output RGB directly. Vulkan format (B8G8R8A8) and DRM format (XRGB8888)
    // will natively align the bytes correctly for the physical display.
    // Multiply the surface's own per-pixel alpha by the window opacity so a
    // surface that is partly transparent (e.g. the desktop shell, whose empty
    // desktop area is cleared to alpha 0 to let app windows below it show
    // through) blends correctly. Opaque apps have texColor.a == 1, so this is
    // unchanged for them.
    outColor = vec4(texColor.rgb, texColor.a * inOpacity);
}
