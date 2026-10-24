#include "xserver/window_decorator.h"
#include "xrender/font_atlas.h"
#include "xrender/font_manager.h"
#include "xrender/text_shaper.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace fbx::xserver {

// ── Primitive helpers ─────────────────────────────────────────────────────────

void WindowDecorator::drawRect(VkCommandBuffer cmd,
                               float x, float y, float w, float h,
                               float r, float g, float b, float a) noexcept {
    // Solid push-constant layout (40 bytes used, pipeline_layout_flat_ supports 48):
    //   vec2 pos, vec2 size, vec2 screen_size, vec4 color
    float pc[10] = { x, y, w, h, (float)screen_w_, (float)screen_h_, r, g, b, a };
    vkCmdPushConstants(cmd, solid_layout_,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, 40, pc);
    vkCmdDraw(cmd, 4, 1, 0, 0);
}

void WindowDecorator::drawRoundedRect(VkCommandBuffer cmd,
                                      float x, float y, float w, float h, float radius,
                                      float r, float g, float b, float a) noexcept {
    if (rounded_pipeline_ == VK_NULL_HANDLE) {
        // Fallback to solid rect if rounded pipeline not yet wired up.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, solid_pipeline_);
        drawRect(cmd, x, y, w, h, r, g, b, a);
        return;
    }
    // Rounded push-constant layout (48 bytes, matches pipeline_layout_flat_):
    //   vec2 pos, vec2 size, vec2 screen_size, float radius, float pad, vec4 color
    float pc[12] = { x, y, w, h,
                     (float)screen_w_, (float)screen_h_,
                     radius, 0.0f,
                     r, g, b, a };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rounded_pipeline_);
    vkCmdPushConstants(cmd, solid_layout_,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, 48, pc);
    vkCmdDraw(cmd, 4, 1, 0, 0);
}

void WindowDecorator::drawCircle(VkCommandBuffer cmd,
                                  float x, float y, float diameter,
                                  float r, float g, float b, float a) noexcept {
    drawRoundedRect(cmd, x, y, diameter, diameter, diameter * 0.5f, r, g, b, a);
}

// ── Main decoration pass ──────────────────────────────────────────────────────

std::expected<void, DecorationError> WindowDecorator::decorate(
    VkCommandBuffer cmd,
    const Window& window,
    VkDescriptorSet /*unused*/,
    bool is_focused) noexcept {

    if (window.is_shell_ || window.is_minimized_ || window.is_subsurface_ || window.layer_ >= 0)
        return {};

    const float x    = window.position_.x_;
    const float y    = window.position_.y_;
    const float w    = window.size_.width_;
    const float h    = window.size_.height_;  // client-area height (may be 0 initially)
    const float by   = y - kTitleBarHeight;   // title bar top

    // ── Drop shadow ───────────────────────────────────────────────────────────
    // Layered translucent rects simulating a soft Gaussian drop shadow.
    // Expanded further downward than sideways (macOS shadow offset ≈ (0, +4)).
    const float shadow_mul = is_focused ? 1.0f : 0.45f;
    const float win_h = kTitleBarHeight + std::max(h, 0.0f);

    struct ShadowLayer { float expand; float dy; float alpha; };
    constexpr ShadowLayer kShadows[] = {
        { 22.0f, 8.0f, 0.040f },
        { 14.0f, 5.0f, 0.075f },
        {  7.0f, 3.0f, 0.130f },
        {  3.0f, 1.5f, 0.200f },
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, solid_pipeline_);
    for (const auto& s : kShadows) {
        drawRect(cmd,
            x  - s.expand,
            by - s.expand + s.dy,
            w  + s.expand * 2.0f,
            win_h + s.expand * 2.0f,
            0.0f, 0.0f, 0.0f, s.alpha * shadow_mul);
    }

    // ── Title bar colour ──────────────────────────────────────────────────────
    // Active: dark cool-grey matching macOS dark mode (#303032 ≈ 0.19/0.19/0.20).
    // Inactive: noticeably dimmer (#232325).
    const float bar_r = is_focused ? 0.19f : 0.14f;
    const float bar_g = is_focused ? 0.19f : 0.14f;
    const float bar_b = is_focused ? 0.20f : 0.15f;
    const float bar_a = is_focused ? 0.97f : 0.92f;

    // ── Title bar: rounded top corners, square bottom ─────────────────────────
    // 1. Full rounded rect (provides the antialiased top curve).
    drawRoundedRect(cmd, x, by, w, kTitleBarHeight, kCornerRadius,
                    bar_r, bar_g, bar_b, bar_a);
    // 2. Solid rect covering the bottom kCornerRadius pixels → squares the bottom edge.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, solid_pipeline_);
    if (kCornerRadius > 0.0f)
        drawRect(cmd, x, y - kCornerRadius, w, kCornerRadius, bar_r, bar_g, bar_b, bar_a);

    // ── Separator line (1 px) ─────────────────────────────────────────────────
    const float sep_a = is_focused ? 0.45f : 0.22f;
    drawRect(cmd, x, y - 1.0f, w, 1.0f, 0.36f, 0.36f, 0.38f, sep_a);

    // ── Traffic-light buttons ─────────────────────────────────────────────────
    // Circles positioned at kBtnCX0, +kBtnSpacing, +kBtnSpacing*2 from window left,
    // vertically centred in the title bar.
    const float btn_y = by + (kTitleBarHeight - kButtonDiameter) * 0.5f; // top of circle

    if (is_focused) {
        drawCircle(cmd, x + kCloseX,    btn_y, kButtonDiameter, 0.988f, 0.373f, 0.337f, 1.0f);
        drawCircle(cmd, x + kMinimizeX, btn_y, kButtonDiameter, 1.000f, 0.737f, 0.180f, 1.0f);
        drawCircle(cmd, x + kMaximizeX, btn_y, kButtonDiameter, 0.157f, 0.784f, 0.251f, 1.0f);
    } else {
        constexpr float gi = 0.37f;
        drawCircle(cmd, x + kCloseX,    btn_y, kButtonDiameter, gi, gi, gi, 1.0f);
        drawCircle(cmd, x + kMinimizeX, btn_y, kButtonDiameter, gi, gi, gi, 1.0f);
        drawCircle(cmd, x + kMaximizeX, btn_y, kButtonDiameter, gi, gi, gi, 1.0f);
    }

    // ── Title text ────────────────────────────────────────────────────────────
    drawTitle(cmd, window, is_focused);

    return {};
}

// ── Text resources ────────────────────────────────────────────────────────────

void WindowDecorator::setTitleTextResources(VkPipeline pipeline, VkPipelineLayout layout,
                                             VkDescriptorSet desc_set,
                                             const xrender::FontAtlas* atlas,
                                             xrender::TextShaper* shaper) noexcept {
    title_pipeline_  = pipeline;
    title_layout_    = layout;
    title_desc_set_  = desc_set;
    title_atlas_     = atlas;
    title_shaper_    = shaper;
}

// ── Title text rendering ──────────────────────────────────────────────────────

void WindowDecorator::drawTitle(VkCommandBuffer cmd, const Window& window,
                                 bool is_focused) noexcept {
    if (!title_pipeline_ || !title_shaper_ || !title_atlas_ || window.title_.empty()) return;

    constexpr float kFontSize = 12.0f;
    constexpr float kAtlasRef = 48.0f;
    const float scale = kFontSize / kAtlasRef;
    constexpr float atlas_w = static_cast<float>(xrender::GlyphCache::kPageSize);
    constexpr float atlas_h = static_cast<float>(xrender::GlyphCache::kPageSize);

    auto* face = xrender::FontManager::getFace();
    auto glyphs = (face && title_shaper_)
        ? title_shaper_->shapeText(window.title_, face->hb_font_, face->face_id_)
        : std::vector<xrender::ShapedGlyph>{};

    // Measure total advance width for horizontal centering.
    float text_w = 0.0f;
    for (const auto& g : glyphs) text_w += g.x_advance_ * scale;

    // Centre in the title bar; clamp left edge so it never overlaps the buttons.
    const float min_x = window.position_.x_ + kMaximizeX + kButtonDiameter + 10.0f;
    float pen_x = window.position_.x_ + (window.size_.width_ - text_w) * 0.5f;
    pen_x = std::max(pen_x, min_x);

    // Vertical centre of the title bar.
    const float pen_y = window.position_.y_ - kTitleBarHeight
                      + kFontSize
                      + (kTitleBarHeight - kFontSize) * 0.5f;

    // Active: near-white.  Inactive: dimmed cool grey (macOS style).
    const float tr = is_focused ? 0.95f : 0.50f;
    const float tg = is_focused ? 0.95f : 0.50f;
    const float tb = is_focused ? 0.97f : 0.52f;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, title_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        title_layout_, 0, 1, &title_desc_set_, 0, nullptr);

    for (const auto& glyph : glyphs) {
        const auto* m = title_atlas_->lookup(glyph.face_id_, glyph.glyph_index_);
        if (!m || m->width_ == 0.0f) { pen_x += glyph.x_advance_ * scale; continue; }

        float gx = pen_x + (m->bearing_x_ + glyph.x_offset_) * scale;
        float gy = pen_y - (m->bearing_y_ + glyph.y_offset_) * scale;
        float gw = m->width_  * scale;
        float gh = m->height_ * scale;

        float pc[14] = {
            gx, gy, gw, gh,
            static_cast<float>(screen_w_), static_cast<float>(screen_h_),
            tr, tg, tb, 1.0f,
            m->atlas_u_, m->atlas_v_,
            m->width_ / atlas_w, m->height_ / atlas_h
        };
        vkCmdPushConstants(cmd, title_layout_,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, 56, pc);
        vkCmdDraw(cmd, 4, 1, 0, 0);
        pen_x += glyph.x_advance_ * scale;
    }
}

} // namespace fbx::xserver
