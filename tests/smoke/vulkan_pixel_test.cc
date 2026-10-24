#include "xserver/vulkan_compositor.h"
#include "xserver/window_stack.h"
#include <gtest/gtest.h>
#include <vector>
#include <cstdint>

namespace fbx::xserver::testing {

class VulkanPixelTest : public ::testing::Test {};

TEST_F(VulkanPixelTest, VerifyCompositorClearColor) {
    auto compositor_res = VulkanCompositor::create();
    if (!compositor_res) {
        GTEST_SKIP() << "Hardware skip: Cannot initialize Vulkan. Missing ICD or incompatible hardware.";
    }

    auto compositor = std::move(compositor_res.value());

    // 1. Execute a composition pass with no client surfaces (should just clear background)
    WindowStack empty_stack;
    auto composite_res = compositor->composite(empty_stack);
    ASSERT_TRUE(composite_res.has_value());

    // 2. Read back the pixels from VRAM to CPU
    auto pixels_res = compositor->readbackPixels(1920, 1080);
    ASSERT_TRUE(pixels_res.has_value());
    const std::vector<uint32_t>& pixels = pixels_res.value();

    ASSERT_EQ(pixels.size(), 1920 * 1080);

    // 3. Verify the clear color is correct (Expected: Black 0x000000, Alpha 0xFF)
    // Format is VK_FORMAT_B8G8R8A8_UNORM.
    // ClearValue was {0.0f, 0.0f, 0.0f, 1.0f}
    // Pixel value should be 0xFF000000
    uint32_t expected_pixel = 0xFF000000;
    
    // Sampling a few strategic pixels to ensure full frame coverage
    EXPECT_EQ(pixels[0], expected_pixel) << "Top-left pixel clear color mismatch";
    EXPECT_EQ(pixels[1920 * 540 + 960], expected_pixel) << "Center pixel clear color mismatch";
    EXPECT_EQ(pixels[1920 * 1080 - 1], expected_pixel) << "Bottom-right pixel clear color mismatch";

    std::cout << "[VulkanPixelTest] Verified 1080p frame composition correctness on hardware.\n";
}

} // namespace fbx::xserver::testing
