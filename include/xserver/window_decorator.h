#pragma once

#include <vulkan/vulkan.h>
#include <memory>
#include "xserver/window_stack.h"
#include "xrender/font_atlas.h"
#include "xrender/text_shaper.h"
#include <expected>

namespace fbx::xserver {

enum class DecorationError {
    PipelineCreationFailed,
    BufferCreationFailed
};

// ── Geometry constants ────────────────────────────────────────────────────────
// All offsets are relative to window.position_ (top-left of the client area).

constexpr float kTitleBarHeight = 28.0f;
constexpr float kCornerRadius   = 10.0f;  // macOS-style rounded window top corners
constexpr float kButtonDiameter = 12.0f;  // traffic-light circle diameter
constexpr float kBtnCX0         = 16.0f;  // close button centre x from window left
constexpr float kBtnSpacing     = 20.0f;  // centre-to-centre between buttons
constexpr float kResizeHandle   =  6.0f;  // hit-zone width for resize edges

// Hit-test bounds (top-left of each button's bounding box, relative to window.position_).
// Used by main.cc input handling — keep in sync with drawing constants above.
constexpr float kCloseX    = kBtnCX0 - kButtonDiameter * 0.5f;               //  10.0f
constexpr float kMinimizeX = kBtnCX0 + kBtnSpacing     - kButtonDiameter * 0.5f; //  30.0f
constexpr float kMaximizeX = kBtnCX0 + kBtnSpacing * 2 - kButtonDiameter * 0.5f; //  50.0f
constexpr float kButtonSize = kButtonDiameter;                                 //  12.0f
// Top of the button bounding box, relative to window.position_.y_
constexpr float kButtonY   = -(kTitleBarHeight / 2.0f + kButtonDiameter / 2.0f); // -20.0f

// ── WindowDecorator ───────────────────────────────────────────────────────────

class WindowDecorator {
public:
    explicit WindowDecorator(
        VkDevice device,
        VkRenderPass render_pass,
        VkPipeline solid_pipeline,
        VkPipelineLayout solid_layout,
        uint32_t screen_w,
        uint32_t screen_h) noexcept
        : device_(device), render_pass_(render_pass),
          solid_pipeline_(solid_pipeline), solid_layout_(solid_layout),
          screen_w_(screen_w), screen_h_(screen_h) {}

    // Renders macOS-style SSD chrome (shadow, title bar, traffic lights, title).
    // is_focused: true when this window has keyboard focus (colours the traffic lights).
    [[nodiscard]] std::expected<void, DecorationError> decorate(
        VkCommandBuffer cmd,
        const Window& window,
        VkDescriptorSet windowDescriptor,
        bool is_focused = true) noexcept;

    void setTitleTextResources(VkPipeline pipeline, VkPipelineLayout layout,
                                VkDescriptorSet desc_set,
                                const xrender::FontAtlas* atlas,
                                xrender::TextShaper* shaper) noexcept;

    // Registers the rounded-rect pipeline (uses the same pipeline_layout_flat_ 48-byte layout).
    void setRoundedPipeline(VkPipeline pipeline) noexcept { rounded_pipeline_ = pipeline; }

private:
    // Draws a solid axis-aligned rectangle.  Caller must have bound solid_pipeline_ first.
    void drawRect(VkCommandBuffer cmd,
                  float x, float y, float w, float h,
                  float r, float g, float b, float a) noexcept;

    // Draws a rounded rectangle with per-pixel SDF anti-aliasing.
    // Binds rounded_pipeline_ internally.
    void drawRoundedRect(VkCommandBuffer cmd,
                         float x, float y, float w, float h, float radius,
                         float r, float g, float b, float a) noexcept;

    // Draws a filled circle (special case of rounded rect with radius = diameter/2).
    void drawCircle(VkCommandBuffer cmd,
                    float x, float y, float diameter,
                    float r, float g, float b, float a) noexcept;

    // Renders the window title, centred in the title bar.
    // is_focused controls text colour (bright vs. dimmed).
    void drawTitle(VkCommandBuffer cmd, const Window& window, bool is_focused) noexcept;

    [[maybe_unused]] VkDevice         device_;
    [[maybe_unused]] VkRenderPass     render_pass_;
    VkPipeline       solid_pipeline_;
    VkPipelineLayout solid_layout_;
    uint32_t         screen_w_;
    uint32_t         screen_h_;

    VkPipeline rounded_pipeline_{VK_NULL_HANDLE};

    VkPipeline            title_pipeline_{VK_NULL_HANDLE};
    VkPipelineLayout      title_layout_{VK_NULL_HANDLE};
    VkDescriptorSet       title_desc_set_{VK_NULL_HANDLE};
    const xrender::FontAtlas*  title_atlas_{nullptr};
    xrender::TextShaper*       title_shaper_{nullptr};
};

} // namespace fbx::xserver
