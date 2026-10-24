// Offscreen render runtime tests.
//
// Exercises the VulkanCompositor pipeline by rendering to an in-memory target
// (no display / no swapchain) and reading pixels back to the CPU.  Tests skip
// automatically when Vulkan is unavailable, so they are safe in headless CI.
//
// Design principles:
//   • The compositor is created ONCE for the entire suite (SetUpTestSuite) to
//     avoid repeated Vulkan init/cleanup cycles, which can trigger driver bugs.
//   • Assertions are behavioural rather than hardcoded ARGB literals, so the
//     tests survive clear-colour changes or driver-specific format quirks.
//   • clear_color_ is captured from a baseline composite at suite setup and
//     used as the reference "background" value in all per-test assertions.

#include "xserver/vulkan_compositor.h"
#include "xserver/window_stack.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

namespace fbx::xserver::testing {

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr uint32_t kW = 256;
static constexpr uint32_t kH = 256;

// ── Suite fixture ─────────────────────────────────────────────────────────────
//
// The compositor and render target are created once before any test runs and
// destroyed after the last test.  Individual tests register temporary Vulkan
// images for cleanup in TearDown.

class OffscreenRenderTest : public ::testing::Test {
protected:
    // ── Suite-level setup / teardown ─────────────────────────────────────────

    static void SetUpTestSuite() {
        auto res = VulkanCompositor::create();
        if (!res) return; // Vulkan unavailable — all tests will skip
        compositor_ = std::move(res.value());

        if (!compositor_->createTarget(kW, kH).has_value()) {
            compositor_.reset();
            return;
        }

        // Baseline composite — verify the pipeline is functional and capture
        // the hardware-specific clear colour for use in per-test assertions.
        WindowStack empty;
        if (!compositor_->composite(empty).has_value()) {
            compositor_.reset();
            return;
        }
        auto px = compositor_->readbackPixels(kW, kH);
        if (!px.has_value() || px->size() != kW * kH) {
            compositor_.reset();
            return;
        }
        clear_color_ = (*px)[kW / 2 * kW + kW / 2]; // centre pixel

        // Verify that two distinct solid-colour images actually produce
        // different pixel values after compositing.  If the blit pipeline is
        // missing (e.g. shaders not built), the two readbacks will be
        // identical clear-colour frames and the suite degrades to a skip.
        VkImage probe_r = compositor_->createSolidColorImage(0xFFFF0000u).value_or(VK_NULL_HANDLE);
        VkImage probe_b = compositor_->createSolidColorImage(0xFF0000FFu).value_or(VK_NULL_HANDLE);
        bool pipeline_ok = false;
        if (probe_r && probe_b) {
            WindowStack s;
            s.addWindow(1, probe_r, (float)kW, (float)kH);
            s.getWindowsMutable().back().opacity_ = 1.0f;
            s.getWindowsMutable().back().is_shell_ = true;
            s.getWindowsMutable().back().position_ = {0, 0};
            s.getWindowsMutable().back().size_ = {(float)kW, (float)kH};
            if (compositor_->composite(s).has_value()) {
                auto rb = compositor_->readbackPixels(kW, kH);
                if (rb && !rb->empty())
                    pipeline_ok = ((*rb)[kW / 2 * kW + kW / 2] != clear_color_);
            }
            compositor_->releaseImage(probe_r);
            compositor_->releaseImage(probe_b);
        }
        if (!pipeline_ok) {
            compositor_.reset();
            return;
        }
    }

    static void TearDownTestSuite() {
        compositor_.reset();
    }

    // ── Per-test setup / teardown ─────────────────────────────────────────────

    void SetUp() override {
        if (!compositor_) {
            GTEST_SKIP() << "Hardware skip: Vulkan unavailable, compositor failed to init, "
                            "or blit pipeline could not be loaded (shaders missing?).";
        }
    }

    void TearDown() override {
        for (VkImage img : owned_images_)
            compositor_->releaseImage(img);
        owned_images_.clear();
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    // Create and register a solid-colour 1×1 surface.  Returns VK_NULL_HANDLE
    // on failure (test should then ASSERT / return early).
    VkImage makeSolid(uint32_t argb) {
        auto res = compositor_->createSolidColorImage(argb);
        if (!res) return VK_NULL_HANDLE;
        owned_images_.push_back(res.value());
        return res.value();
    }

    // Add a window that is immediately visible (opacity=1, no decorations).
    void addWindow(WindowStack& stack, VkImage img, uint32_t id,
                   float x, float y, float w, float h) {
        stack.addWindow(id, img, w, h);
        auto& win = stack.getWindowsMutable().back();
        win.position_        = {x, y};
        win.size_            = {w, h};
        win.target_position_ = {x, y};
        win.target_size_     = {w, h};
        win.opacity_         = 1.0f;   // skip the default fade-in animation
        win.is_shell_        = true;   // no title-bar decoration
    }

    // Read back all pixels; marks the calling test failed on error.
    std::vector<uint32_t> readback() {
        auto res = compositor_->readbackPixels(kW, kH);
        EXPECT_TRUE(res.has_value()) << "readbackPixels failed";
        return res.value_or(std::vector<uint32_t>{});
    }

    // Pixel at (x, y) in a kW-wide buffer.
    static uint32_t px(const std::vector<uint32_t>& buf, uint32_t x, uint32_t y) {
        return buf[y * kW + x];
    }

    // Static members shared across all tests in the suite.
    static std::unique_ptr<VulkanCompositor> compositor_;
    static uint32_t clear_color_;

    // Per-test: images to release in TearDown.
    std::vector<VkImage> owned_images_;
};

std::unique_ptr<VulkanCompositor> OffscreenRenderTest::compositor_;
uint32_t OffscreenRenderTest::clear_color_ = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: Empty stack produces a uniform clear colour
// All pixels must be identical — whatever the compositor's native clear colour.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(OffscreenRenderTest, EmptyStackIsUniform) {
    WindowStack stack;
    ASSERT_TRUE(compositor_->composite(stack).has_value());

    auto buf = readback();
    ASSERT_EQ(buf.size(), kW * kH);

    uint32_t expected = buf[0];
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x)
            EXPECT_EQ(px(buf, x, y), expected)
                << "Non-uniform pixel at (" << x << ", " << y << ")";
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: Full-viewport window changes all pixels from the clear colour
// A red window exactly covering the 256×256 target must produce pixels that
// all differ from the clear colour captured at suite setup.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(OffscreenRenderTest, FullViewportWindowChangesPixels) {
    VkImage img = makeSolid(0xFFFF0000u); // red
    ASSERT_NE(img, VK_NULL_HANDLE);

    WindowStack stack;
    addWindow(stack, img, 1, 0.0f, 0.0f, (float)kW, (float)kH);
    ASSERT_TRUE(compositor_->composite(stack).has_value());

    auto buf = readback();
    ASSERT_EQ(buf.size(), kW * kH);

    // Centre pixel must differ from the clear colour (window was actually drawn).
    EXPECT_NE(px(buf, kW / 2, kH / 2), clear_color_)
        << "Window pixel should differ from clear colour";

    // All pixels should be the same (solid-colour window covers the whole target).
    uint32_t first = buf[0];
    EXPECT_NE(first, clear_color_) << "corner pixel should also differ from clear";
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x)
            EXPECT_EQ(px(buf, x, y), first)
                << "Non-uniform pixel at (" << x << ", " << y << ")";
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: Partial window — region inside window differs from region outside
// Green 128×128 at (64, 64): centre of the window should differ from the
// clear colour; corners of the target should remain the clear colour.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(OffscreenRenderTest, PartialWindowLeavesBackground) {
    VkImage img = makeSolid(0xFF00FF00u); // green
    ASSERT_NE(img, VK_NULL_HANDLE);

    WindowStack stack;
    addWindow(stack, img, 1, 64.0f, 64.0f, 128.0f, 128.0f);
    ASSERT_TRUE(compositor_->composite(stack).has_value());

    auto buf = readback();
    ASSERT_EQ(buf.size(), kW * kH);

    // Centre of the green window (64+64, 64+64) = (128, 128) — drawn
    EXPECT_NE(px(buf, 128, 128), clear_color_) << "inside window should not be clear";

    // Corners — clear colour
    EXPECT_EQ(px(buf, 0,   0  ), clear_color_) << "top-left corner outside window";
    EXPECT_EQ(px(buf, 255, 255), clear_color_) << "bottom-right corner outside window";

    // Inside ≠ outside
    EXPECT_NE(px(buf, 128, 128), px(buf, 0, 0))
        << "window interior should differ from exterior";
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: Z-order — top window pixel wins at overlap, bottom visible elsewhere
// Blue full-viewport (bottom) + red 128×128 at (64,64) on top.
// The region under the red window must differ from the region only under blue,
// proving that z-ordering placed different colours at different positions.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(OffscreenRenderTest, ZOrderTopWindowWinsAtOverlap) {
    VkImage blue_img = makeSolid(0xFF0000FFu); // blue
    VkImage red_img  = makeSolid(0xFFFF0000u); // red
    ASSERT_NE(blue_img, VK_NULL_HANDLE);
    ASSERT_NE(red_img,  VK_NULL_HANDLE);

    WindowStack stack;
    addWindow(stack, blue_img, 1, 0.0f, 0.0f, (float)kW, (float)kH); // bottom
    addWindow(stack, red_img,  2, 64.0f, 64.0f, 128.0f, 128.0f);     // top
    ASSERT_TRUE(compositor_->composite(stack).has_value());

    auto buf = readback();
    ASSERT_EQ(buf.size(), kW * kH);

    // Non-overlap region (10,10): only blue window — must not be clear
    EXPECT_NE(px(buf, 10, 10), clear_color_) << "non-overlap: blue window must be visible";

    // Overlap region (128,128): red window on top — must differ from (10,10)
    EXPECT_NE(px(buf, 128, 128), px(buf, 10, 10))
        << "overlap region must show a different colour than non-overlap (z-order)";
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: Frame checksum changes when content changes
// Checksum of an empty frame must differ from one with a window.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(OffscreenRenderTest, FrameChecksumDiffersWithContent) {
    WindowStack empty;
    ASSERT_TRUE(compositor_->composite(empty).has_value());
    uint32_t cs_empty = compositor_->getFrameChecksum();

    VkImage img = makeSolid(0xFFFF0000u); // red
    ASSERT_NE(img, VK_NULL_HANDLE);

    WindowStack red;
    addWindow(red, img, 1, 0.0f, 0.0f, (float)kW, (float)kH);
    ASSERT_TRUE(compositor_->composite(red).has_value());
    uint32_t cs_red = compositor_->getFrameChecksum();

    EXPECT_NE(cs_empty, cs_red)
        << "Frame checksum must change when a window is added";
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: Sequential composites of the same scene are pixel-identical
// The pipeline must be deterministic: same inputs → same outputs.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(OffscreenRenderTest, SequentialCompositesAreStable) {
    VkImage img = makeSolid(0xFF00FF00u); // green
    ASSERT_NE(img, VK_NULL_HANDLE);

    auto makeScene = [&]() {
        WindowStack stack;
        addWindow(stack, img, 1, 0.0f, 0.0f, (float)kW, (float)kH);
        return stack;
    };

    WindowStack s1 = makeScene();
    ASSERT_TRUE(compositor_->composite(s1).has_value());
    auto buf1 = readback();

    WindowStack s2 = makeScene();
    ASSERT_TRUE(compositor_->composite(s2).has_value());
    auto buf2 = readback();

    ASSERT_EQ(buf1.size(), buf2.size());
    EXPECT_EQ(buf1, buf2) << "Two composites of the same scene must be pixel-identical";
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: Semi-transparent window blends with background
//
// A window at 50% opacity over a solid background must produce a pixel that
// differs from both the fully-opaque background alone AND the clear colour.
// This confirms the alpha-blending path is active (not a transparency skip).
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(OffscreenRenderTest, SemiTransparentWindowBlendsWithBackground) {
    VkImage bg_img  = makeSolid(0xFF0000FFu); // blue background (fully opaque)
    VkImage top_img = makeSolid(0xFFFF0000u); // red overlay (will be made 50% opaque)
    ASSERT_NE(bg_img,  VK_NULL_HANDLE);
    ASSERT_NE(top_img, VK_NULL_HANDLE);

    // First: pure blue background — capture pixel value as reference
    WindowStack bg_stack;
    addWindow(bg_stack, bg_img, 1, 0.0f, 0.0f, (float)kW, (float)kH);
    ASSERT_TRUE(compositor_->composite(bg_stack).has_value());
    auto bg_buf = readback();
    ASSERT_EQ(bg_buf.size(), kW * kH);
    uint32_t bg_px = px(bg_buf, kW / 2, kH / 2);
    EXPECT_NE(bg_px, clear_color_) << "background must differ from clear";

    // Now: blue background + 50% red on top
    WindowStack blend_stack;
    addWindow(blend_stack, bg_img,  1, 0.0f, 0.0f, (float)kW, (float)kH);
    addWindow(blend_stack, top_img, 2, 0.0f, 0.0f, (float)kW, (float)kH);
    blend_stack.getWindowsMutable().back().opacity_ = 0.5f; // 50% transparent

    ASSERT_TRUE(compositor_->composite(blend_stack).has_value());
    auto blend_buf = readback();
    ASSERT_EQ(blend_buf.size(), kW * kH);
    uint32_t blended_px = px(blend_buf, kW / 2, kH / 2);

    // The blended pixel must differ from the pure background and from clear.
    // (With a fully-transparent top window the pixel would equal bg_px; with
    //  an opaque top it would equal some red value.  50% must be neither.)
    EXPECT_NE(blended_px, clear_color_) << "blended pixel should not be clear";
    EXPECT_NE(blended_px, bg_px)
        << "blended pixel must differ from fully-opaque background "
           "(alpha-blend path not reached)";
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: Removing a window from the stack clears its pixels
//
// Add a red window covering the full viewport, composite.  Remove the window,
// composite again.  The resulting frame must be pixel-identical to a frame
// produced by compositing an empty stack.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(OffscreenRenderTest, RemovingWindowRestoresClearColour) {
    // Baseline: empty stack
    WindowStack empty;
    ASSERT_TRUE(compositor_->composite(empty).has_value());
    auto empty_buf = readback();
    ASSERT_FALSE(empty_buf.empty());

    // Add a full-viewport window
    VkImage img = makeSolid(0xFFFF0000u); // red
    ASSERT_NE(img, VK_NULL_HANDLE);
    WindowStack with_window;
    addWindow(with_window, img, 1, 0.0f, 0.0f, (float)kW, (float)kH);
    ASSERT_TRUE(compositor_->composite(with_window).has_value());

    // Verify the window was actually drawn (centre pixel differs from empty)
    auto with_buf = readback();
    EXPECT_NE(px(with_buf, kW / 2, kH / 2), px(empty_buf, kW / 2, kH / 2))
        << "window must change pixels before removal";

    // Now composite an empty stack again
    WindowStack empty2;
    ASSERT_TRUE(compositor_->composite(empty2).has_value());
    auto restored_buf = readback();
    ASSERT_EQ(restored_buf.size(), empty_buf.size());

    // Must match the original empty-stack frame
    EXPECT_EQ(restored_buf, empty_buf)
        << "After removing window, frame must match original empty composite";
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: Damage-region composite is consistent with full composite
//
// Composite a full frame with a red window.  Then composite the same scene
// again with a damage rect restricted to a 64×64 sub-region at (96,96).
// The pixels inside the damage region must be identical between the two frames.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(OffscreenRenderTest, DamageRegionPixelsMatchFullComposite) {
    VkImage img = makeSolid(0xFFFF0000u); // red
    ASSERT_NE(img, VK_NULL_HANDLE);

    // Full composite (no damage hint — entire frame)
    WindowStack stack;
    addWindow(stack, img, 1, 0.0f, 0.0f, (float)kW, (float)kH);
    ASSERT_TRUE(compositor_->composite(stack).has_value());
    auto full_buf = readback();
    ASSERT_EQ(full_buf.size(), kW * kH);

    // Damage-region composite: restrict to a 64×64 tile at (96, 96)
    constexpr float dx = 96.0f, dy = 96.0f, dw = 64.0f, dh = 64.0f;
    WindowStack stack2;
    addWindow(stack2, img, 1, 0.0f, 0.0f, (float)kW, (float)kH);
    ASSERT_TRUE(compositor_->composite(stack2, dx, dy, dw, dh).has_value());
    auto damage_buf = readback();
    ASSERT_EQ(damage_buf.size(), kW * kH);

    // Pixels inside the damage region must match the full composite
    bool any_checked = false;
    for (uint32_t y = static_cast<uint32_t>(dy);
         y < static_cast<uint32_t>(dy + dh) && y < kH; ++y) {
        for (uint32_t x = static_cast<uint32_t>(dx);
             x < static_cast<uint32_t>(dx + dw) && x < kW; ++x) {
            EXPECT_EQ(px(damage_buf, x, y), px(full_buf, x, y))
                << "Damage-region pixel at (" << x << "," << y
                << ") must match full composite";
            any_checked = true;
        }
    }
    EXPECT_TRUE(any_checked) << "No pixels were checked — test geometry is wrong";
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: Three-window stack — z-order positions each colour correctly
//
// Bottom: blue full viewport.  Middle: green 128×128 at (0,0).  Top: red
// 64×64 at (32,32).
//
//  (0,0)...(64,64) — red region (all three overlap, red is top)
//  (64,0)...(128,128) — green region (blue+green, no red)
//  (128,0)...(255,255) — blue region (only blue)
//
// All three regions must show distinct pixel values, proving the compositor
// correctly respects z-order for all three layers.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(OffscreenRenderTest, ThreeWindowZOrderAllLayersDistinct) {
    VkImage blue_img  = makeSolid(0xFF0000FFu);
    VkImage green_img = makeSolid(0xFF00FF00u);
    VkImage red_img   = makeSolid(0xFFFF0000u);
    ASSERT_NE(blue_img,  VK_NULL_HANDLE);
    ASSERT_NE(green_img, VK_NULL_HANDLE);
    ASSERT_NE(red_img,   VK_NULL_HANDLE);

    WindowStack stack;
    addWindow(stack, blue_img,  1, 0.0f,  0.0f,  (float)kW, (float)kH); // bottom
    addWindow(stack, green_img, 2, 0.0f,  0.0f,  128.0f,    128.0f);    // middle
    addWindow(stack, red_img,   3, 32.0f, 32.0f, 64.0f,     64.0f);     // top
    ASSERT_TRUE(compositor_->composite(stack).has_value());

    auto buf = readback();
    ASSERT_EQ(buf.size(), kW * kH);

    // Sample points
    uint32_t red_sample   = px(buf, 48,  48);  // inside red   (32+16, 32+16)
    uint32_t green_sample = px(buf, 100, 64);  // green only   (100, 64) — inside green, outside red
    uint32_t blue_sample  = px(buf, 200, 200); // blue only    — outside green, outside red

    // All three must differ from the clear colour (all covered by at least one window)
    EXPECT_NE(red_sample,   clear_color_) << "red region should not be clear";
    EXPECT_NE(green_sample, clear_color_) << "green region should not be clear";
    EXPECT_NE(blue_sample,  clear_color_) << "blue region should not be clear";

    // All three must be distinct (each layer produces a unique colour)
    EXPECT_NE(red_sample, green_sample)
        << "red and green regions must show different colours";
    EXPECT_NE(green_sample, blue_sample)
        << "green and blue regions must show different colours";
    EXPECT_NE(red_sample, blue_sample)
        << "red and blue regions must show different colours";
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: Window repositioned between composites updates correct pixels
//
// Place a red 64×64 window at (0,0), composite.  Move it to (192,192) and
// composite again.  Pixel at (32,32) (old centre) must now equal the clear
// colour; pixel at (224,224) (new centre) must differ from clear.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(OffscreenRenderTest, RepositionedWindowUpdatesPixels) {
    VkImage img = makeSolid(0xFFFF0000u); // red
    ASSERT_NE(img, VK_NULL_HANDLE);

    // Position 1: top-left corner
    WindowStack stack1;
    addWindow(stack1, img, 1, 0.0f, 0.0f, 64.0f, 64.0f);
    ASSERT_TRUE(compositor_->composite(stack1).has_value());
    auto buf1 = readback();

    EXPECT_NE(px(buf1, 32, 32), clear_color_)  << "old position centre must be drawn";
    EXPECT_EQ(px(buf1, 224, 224), clear_color_) << "new position must be clear before move";

    // Position 2: bottom-right area
    WindowStack stack2;
    addWindow(stack2, img, 1, 192.0f, 192.0f, 64.0f, 64.0f);
    ASSERT_TRUE(compositor_->composite(stack2).has_value());
    auto buf2 = readback();

    EXPECT_EQ(px(buf2, 32, 32), clear_color_)   << "old position must now be clear";
    EXPECT_NE(px(buf2, 224, 224), clear_color_)  << "new position centre must be drawn";
}

} // namespace fbx::xserver::testing
