#pragma once

#include "xkit/layout.h"
#include "xserver/window_layout.h"
#include "shared/protocol.h"
#include <vector>
#include <optional>
#include <algorithm>
#include <unordered_map>
#include <string>
#include <vulkan/vulkan.h>
#include <iostream>

namespace fbx::xserver {

enum class LayoutMode {
    Floating,
    Tiling,
    MissionControl
};

// Describes one output in global (desktop-wide) logical coordinate space.
// Physical pixels = logical * scale_. Outputs are laid out horizontally.
struct OutputInfo {
    float x_offset_{0.0f};  // logical pixels from left edge
    float width_{1920.0f};  // logical pixels
    float height_{1080.0f}; // logical pixels
    float scale_{1.0f};     // HiDPI scale factor (1.0 = no scaling, 2.0 = Retina)
    // Physical dimensions (set separately, not part of layout):
    float phys_width_{1920.0f};
    float phys_height_{1080.0f};
    // VRR (Variable Refresh Rate / FreeSync / G-Sync / AdaptiveSync):
    bool  vrr_capable_{false}; // hardware supports DRM VRR_ENABLED property
    bool  vrr_enabled_{false}; // VRR is currently active on this output
    // HDR (High Dynamic Range):
    bool     hdr_capable_{false};       // hardware supports HDR_OUTPUT_METADATA connector property
    bool     hdr_enabled_{false};       // HDR is currently active on this output
    uint32_t hdr_max_luminance_{0};     // peak luminance in nits (0 = not configured)
};

struct Window {
    uint32_t surface_id_;
    VkImage surface_image_{VK_NULL_HANDLE};
    xkit::Offset position_{0, 0};       // global coordinates
    xkit::Size size_{0, 0};
    xkit::Offset target_position_{0, 0};
    xkit::Size target_size_{0, 0};

    xkit::Offset velocity_{0, 0};
    xkit::Size size_velocity_{0, 0};

    float scale_{1.0f};
    float target_scale_{1.0f};
    float scale_velocity_{0.0f};

    float opacity_{1.0f};
    float target_opacity_{1.0f};
    float opacity_velocity_{0.0f};

    int8_t layer_{-1};           // -1 = not a layer surface; 0-3 = zwlr_layer_shell layer
    bool is_modal_{false};       // xdg_dialog_v1: blocks input to all other windows
    bool is_shell_{false};
    bool is_subsurface_{false};  // wl_subsurface: no decorations, position tracks parent
    uint32_t parent_id_{0};      // parent surface_id (valid when is_subsurface_)
    float    sub_x_{0.0f};       // offset from parent's top-left corner
    float    sub_y_{0.0f};
    bool is_dragging_{false};
    bool is_minimized_{false};
    bool is_maximized_{false};
    uint32_t toplevel_parent_id_{0};     // xdg_toplevel parent surface id (0 = none)
    bool     is_resizing_{false};
    uint32_t resize_edges_{0};           // XDG resize edge bitmask (L=4,R=8,B=2)
    xkit::Offset resize_cursor_start_{0, 0};
    xkit::Offset resize_pos_start_{0, 0};
    xkit::Size   resize_size_start_{0, 0};
    xkit::Offset drag_offset_{0, 0};

    xkit::Offset damage_pos_{0, 0};
    xkit::Size damage_size_{0, 0};

    struct {
        xkit::Offset position_{0, 0};
        xkit::Size size_{0, 0};
    } restore_rect_;

    uint32_t output_idx_{0};  // which physical output this window lives on
    std::string title_{""};   // window title bar text (UTF-8)

    // xdg_surface.set_window_geometry: content rect within the buffer.
    // geom_w_ == 0 means not set; buffer covers the full surface area.
    // When set, the buffer is rendered at (position_ - geom_{x,y}) so that
    // the geometry top-left aligns with position_. Shadows outside the
    // geometry rect are visible but do not receive SSD chrome.
    int32_t geom_x_{0}, geom_y_{0}, geom_w_{0}, geom_h_{0};
};

class Workspace {
public:
    Workspace() = default;

    std::vector<Window> windows_;
    LayoutMode layout_mode_{LayoutMode::Floating};
    float  master_ratio_{0.6f};   // master-column fraction in Tiling mode [0.1,0.9]
    size_t master_count_{1};      // windows in the master column

    // Tiles windows per-output using the provided output geometry.
    void applyTiling(const std::vector<OutputInfo>& outputs) noexcept {
        if (windows_.empty() || outputs.empty()) return;

        if (layout_mode_ == LayoutMode::Tiling) {
            // Master/stack tiling per output: the first window is the master
            // (master_ratio_ of the width), the rest stack in the side column.
            // Geometry is the pure layout::masterStack() helper (unit-tested).
            constexpr float kTopBar = 28.0f;  // reserve the shell top bar
            for (size_t oi = 0; oi < outputs.size(); ++oi) {
                const auto& out = outputs[oi];
                std::vector<Window*> bucket;
                for (auto& w : windows_) {
                    if (!w.is_shell_ && !w.is_minimized_ && w.layer_ < 0 && w.output_idx_ == oi)
                        bucket.push_back(&w);
                }
                if (bucket.empty()) continue;
                auto rects = layout::masterStack(
                    bucket.size(), out.x_offset_, kTopBar,
                    out.width_, out.height_ - kTopBar, master_ratio_, master_count_);
                for (size_t i = 0; i < bucket.size(); ++i) {
                    bucket[i]->target_position_ = {rects[i].x, rects[i].y};
                    bucket[i]->target_size_     = {rects[i].w, rects[i].h};
                    bucket[i]->target_scale_    = 1.0f;
                }
            }
        } else if (layout_mode_ == LayoutMode::MissionControl) {
            // Mission-control overview across each output independently.
            for (size_t oi = 0; oi < outputs.size(); ++oi) {
                const auto& out = outputs[oi];
                std::vector<Window*> bucket;
                for (auto& w : windows_) {
                    if (!w.is_shell_ && !w.is_minimized_ && w.layer_ < 0 && w.output_idx_ == oi)
                        bucket.push_back(&w);
                }
                if (bucket.empty()) continue;
                size_t cols = static_cast<size_t>(std::ceil(std::sqrt(static_cast<float>(bucket.size()))));
                float cell_w = out.width_  / static_cast<float>(cols);
                float cell_h = out.height_ / static_cast<float>(cols);
                float padding = 40.0f;
                for (size_t idx = 0; idx < bucket.size(); ++idx) {
                    float x = out.x_offset_ + (idx % cols) * cell_w + padding;
                    float y = (idx / cols) * cell_h + padding;
                    bucket[idx]->target_position_ = {x, y};
                    bucket[idx]->target_scale_    = 0.8f / static_cast<float>(cols);
                }
            }
        } else {
            // Floating: restore scale
            for (auto& w : windows_) {
                if (!w.is_shell_) w.target_scale_ = 1.0f;
            }
        }
    }
};

class WindowStack {
public:
    // Number of virtual workspaces. Single workspace for now; the multi-workspace
    // machinery (switch/slide, per-workspace window lists) is kept intact — raise
    // this (and the shell's kNumWorkspaces) to re-enable multiple workspaces.
    static constexpr int kWorkspaceCount = 1;
    // workspace_count defaults to kWorkspaceCount (production); tests pass a larger
    // count to exercise the multi-workspace paths while the default stays single.
    explicit WindowStack(int workspace_count = kWorkspaceCount) {
        if (workspace_count < 1) workspace_count = 1;
        for (int i = 0; i < workspace_count; ++i) workspaces_.emplace_back();
        // Default single-output layout at 1× scale
        outputs_.push_back({0.0f, 1920.0f, 1080.0f, 1.0f, 1920.0f, 1080.0f});
    }

    // Called on startup and on DRM hot-plug to update the output layout.
    // Preserves per-output scale factors set by the user.
    void setOutputLayout(std::vector<OutputInfo> outputs) noexcept {
        if (outputs.empty()) return;
        // Preserve any scale values already configured for each output
        for (size_t i = 0; i < outputs.size() && i < outputs_.size(); ++i) {
            if (outputs_[i].scale_ != 1.0f)
                outputs[i].scale_ = outputs_[i].scale_;
        }
        outputs_ = std::move(outputs);
        // Recompute logical dims using scale
        for (auto& o : outputs_) {
            o.width_  = o.phys_width_  / o.scale_;
            o.height_ = o.phys_height_ / o.scale_;
        }
        screen_w_ = outputs_[0].width_;
        screen_h_ = outputs_[0].height_;
        updateLayout();
    }

    // Set HiDPI scale for a specific output. Immediately recomputes logical dims.
    void setOutputScale(size_t idx, float scale) noexcept {
        if (idx >= outputs_.size() || scale <= 0.0f) return;
        outputs_[idx].scale_  = scale;
        outputs_[idx].width_  = outputs_[idx].phys_width_  / scale;
        outputs_[idx].height_ = outputs_[idx].phys_height_ / scale;
        // Recompute x_offsets (logical) for all outputs after the changed one
        float x = 0.0f;
        for (auto& o : outputs_) { o.x_offset_ = x; x += o.width_; }
        if (!outputs_.empty()) { screen_w_ = outputs_[0].width_; screen_h_ = outputs_[0].height_; }
        updateLayout();
    }

    [[nodiscard]] float getOutputScale(size_t idx) const noexcept {
        return (idx < outputs_.size()) ? outputs_[idx].scale_ : 1.0f;
    }

    void setVrrCapable(size_t idx, bool capable) noexcept {
        if (idx < outputs_.size()) outputs_[idx].vrr_capable_ = capable;
    }

    void setVrrEnabled(size_t idx, bool enabled) noexcept {
        if (idx < outputs_.size()) outputs_[idx].vrr_enabled_ = enabled;
    }

    [[nodiscard]] bool isVrrCapable(size_t idx) const noexcept {
        return (idx < outputs_.size()) && outputs_[idx].vrr_capable_;
    }

    [[nodiscard]] bool isVrrEnabled(size_t idx) const noexcept {
        return (idx < outputs_.size()) && outputs_[idx].vrr_enabled_;
    }

    void setHdrCapable(size_t idx, bool capable) noexcept {
        if (idx < outputs_.size()) outputs_[idx].hdr_capable_ = capable;
    }
    void setHdrEnabled(size_t idx, bool enabled) noexcept {
        if (idx < outputs_.size()) outputs_[idx].hdr_enabled_ = enabled;
    }
    [[nodiscard]] bool isHdrCapable(size_t idx) const noexcept {
        return (idx < outputs_.size()) && outputs_[idx].hdr_capable_;
    }
    [[nodiscard]] bool isHdrEnabled(size_t idx) const noexcept {
        return (idx < outputs_.size()) && outputs_[idx].hdr_enabled_;
    }

    void setScreenSize(float w, float h) noexcept {
        // w and h here are physical pixels (from DRM); store both physical and logical.
        screen_w_ = w; screen_h_ = h;
        if (!outputs_.empty()) {
            outputs_[0].phys_width_  = w;
            outputs_[0].phys_height_ = h;
            outputs_[0].width_  = w / outputs_[0].scale_;
            outputs_[0].height_ = h / outputs_[0].scale_;
            screen_w_ = outputs_[0].width_;
            screen_h_ = outputs_[0].height_;
        }
    }

    // Returns windows that belong to output_idx (or are shell windows),
    // with positions adjusted to output-local coordinates.
    [[nodiscard]] std::vector<Window> getWindowsForOutput(size_t output_idx) const noexcept {
        const auto& ws = workspaces_[active_workspace_];
        float x_off = (output_idx < outputs_.size()) ? outputs_[output_idx].x_offset_ : 0.0f;

        // Build lookup of current animated positions for subsurface parent resolution.
        std::unordered_map<uint32_t, xkit::Offset> parent_pos;
        for (const auto& w : ws.windows_) {
            if (!w.is_subsurface_)
                parent_pos[w.surface_id_] = w.position_;
        }

        std::vector<Window> result;
        result.reserve(ws.windows_.size());
        for (const auto& w : ws.windows_) {
            if (w.is_subsurface_) {
                auto pit = parent_pos.find(w.parent_id_);
                if (pit == parent_pos.end()) continue; // parent not visible
                Window adjusted = w;
                adjusted.position_.x_ = pit->second.x_ + w.sub_x_ - x_off;
                adjusted.position_.y_ = pit->second.y_ + w.sub_y_;
                result.push_back(std::move(adjusted));
                continue;
            }
            // Layer surfaces and shell surfaces appear on all outputs.
            if (!w.is_shell_ && w.layer_ < 0 && w.output_idx_ != output_idx) continue;
            Window adjusted = w;
            // Shift to output-local coordinates so the compositor renders correctly.
            adjusted.position_.x_        -= x_off;
            adjusted.target_position_.x_ -= x_off;
            result.push_back(std::move(adjusted));
        }
        // Sort by z-order: background(0) → bottom(1) → normal(-1) → top(2) → shell → overlay(3)
        std::stable_sort(result.begin(), result.end(), [](const Window& a, const Window& b) {
            auto z = [](const Window& w) -> int {
                if (w.layer_ == 0) return 0;
                if (w.layer_ == 1) return 10;
                if (w.layer_ == 2) return 30;
                // 998 = kLockSurfaceId: the lock screen sits above everything
                // (desktop shell, apps, overlays) while locked.
                if (w.is_shell_)   return (w.surface_id_ == 998) ? 60 : 40;
                if (w.layer_ == 3) return 50;
                if (w.toplevel_parent_id_ != 0) return 21; // children above normal windows
                return 20; // normal app window
            };
            return z(a) < z(b);
        });
        return result;
    }

    // Legacy accessor used by single-output paths (returns all windows, global coords).
    [[nodiscard]] const std::vector<Window>& getWindows() const noexcept {
        return workspaces_[active_workspace_].windows_;
    }

    [[nodiscard]] std::vector<Window>& getWindowsMutable() noexcept {
        return workspaces_[active_workspace_].windows_;
    }

    // Global top-left of a window's surface by id, for converting the global
    // cursor position into surface-local coordinates. Returns {0,0} for the
    // shell / an unknown id (both are anchored at the origin).
    [[nodiscard]] xkit::Offset windowOrigin(uint32_t surface_id) const noexcept {
        const auto& ws = workspaces_[active_workspace_];
        for (const auto& w : ws.windows_)
            if (w.surface_id_ == surface_id) return w.position_;
        return {0.0f, 0.0f};
    }

    // True while a window is being moved via a title-bar drag. Used to suppress
    // hover cursor changes (e.g. resize-edge feedback) during an active move.
    [[nodiscard]] bool isDragging() const noexcept {
        const auto& ws = workspaces_[active_workspace_];
        return std::any_of(ws.windows_.begin(), ws.windows_.end(),
                           [](const Window& w) { return w.is_dragging_; });
    }

    void addWindow(uint32_t surface_id, VkImage image, float w, float h) noexcept {
        auto& ws = workspaces_[active_workspace_];
        // Place new window on the output that contains the current cursor, centered
        // on that output. Opening centered (rather than pinned to the top-left
        // corner) keeps the window's title-bar close button away from the launcher,
        // where the click that opened the app lands — which otherwise made a stray
        // follow-up click dismiss the window immediately. Clamp so the title bar
        // stays fully on-screen even for a near-fullscreen window.
        uint32_t oi = outputForX(cursor_x_);
        float x_off = (oi < outputs_.size()) ? outputs_[oi].x_offset_ : 0.0f;
        float init_x = x_off + std::max(24.0f, (screen_w_ - w) * 0.5f);
        float init_y = std::max(48.0f, (screen_h_ - h) * 0.5f);
        ws.windows_.push_back({
            .surface_id_      = surface_id,
            .surface_image_   = image,
            .position_        = {init_x, init_y},
            .size_            = {w, h},
            .target_position_ = {init_x, init_y},
            .target_size_     = {w, h},
            .opacity_         = 0.0f,
            .target_opacity_  = 1.0f,
            .restore_rect_    = {{init_x, init_y}, {w, h}},
            .output_idx_      = oi
        });
        // Apply any parent relationship set before this surface was added
        auto pit = pending_parents_.find(surface_id);
        if (pit != pending_parents_.end()) {
            uint32_t parent_id = pit->second;
            pending_parents_.erase(pit);
            setToplevelParent(surface_id, parent_id);
        }
        updateLayout();
    }

    // Add a wl_subsurface. The subsurface is rendered on top of its parent at
    // (parent.position + sub_x, parent.position + sub_y) — resolved each frame.
    void addSubsurface(uint32_t id, uint32_t parent_id, float sub_x, float sub_y) noexcept {
        auto& ws = workspaces_[active_workspace_];
        ws.windows_.push_back({
            .surface_id_      = id,
            .surface_image_   = VK_NULL_HANDLE,
            .position_        = {sub_x, sub_y},
            .size_            = {0, 0},
            .target_position_ = {sub_x, sub_y},
            .target_size_     = {0, 0},
            .opacity_         = 1.0f,
            .target_opacity_  = 1.0f,
            .is_subsurface_   = true,
            .parent_id_       = parent_id,
            .sub_x_           = sub_x,
            .sub_y_           = sub_y,
        });
        // Subsurfaces are not tiled; don't trigger updateLayout()
    }

    // Update a subsurface's offset relative to its parent (from wl_subsurface.set_position).
    void updateSubsurfacePosition(uint32_t id, float sub_x, float sub_y) noexcept {
        for (auto& ws : workspaces_) {
            for (auto& w : ws.windows_) {
                if (w.surface_id_ == id && w.is_subsurface_) {
                    w.sub_x_ = sub_x;
                    w.sub_y_ = sub_y;
                }
            }
        }
    }

    void addShellWindow(uint32_t surface_id, VkImage image, float w, float h) noexcept {
        std::cout << "[window_stack] Adding Shell Window ID: " << surface_id << " to " << workspaces_.size() << " workspace(s).\n";
        for (auto& ws : workspaces_) {
            ws.windows_.push_back({
                .surface_id_      = surface_id,
                .surface_image_   = image,
                .position_        = {0, 0},
                .size_            = {w, h},
                .target_position_ = {0, 0},
                .target_size_     = {w, h},
                .opacity_         = 1.0f,
                .target_opacity_  = 1.0f,
                .is_shell_        = true,
                .restore_rect_    = {{0, 0}, {w, h}}
            });
        }
        updateLayout();
    }

    // Add a layer-shell surface (zwlr_layer_shell_v1). Appears in all workspaces.
    // layer: 0=background, 1=bottom, 2=top, 3=overlay
    void addLayerSurface(uint32_t id, uint32_t layer, float x, float y, float w, float h) noexcept {
        for (auto& ws : workspaces_) {
            ws.windows_.push_back({
                .surface_id_      = id,
                .surface_image_   = VK_NULL_HANDLE,
                .position_        = {x, y},
                .size_            = {w, h},
                .target_position_ = {x, y},
                .target_size_     = {w, h},
                .opacity_         = 1.0f,
                .target_opacity_  = 1.0f,
                .layer_           = static_cast<int8_t>(layer),
            });
        }
    }

    // Mark or unmark a toplevel surface as a modal dialog (xdg_dialog_v1).
    // While any modal surface exists, hit-testing ignores all non-modal windows.
    void setModal(uint32_t surface_id, bool modal) noexcept {
        for (auto& ws : workspaces_) {
            for (auto& w : ws.windows_) {
                if (w.surface_id_ == surface_id)
                    w.is_modal_ = modal;
            }
        }
    }

    // Check if cursor is in a window's resize border. Returns {surface_id, edge_bitmask}.
    // Edges: left=4, right=8, bottom=2, bottom-left=6, bottom-right=10.
    [[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>>
    resizeHitTest(xkit::Offset mouse_pos) const noexcept {
        constexpr float kRH = 6.0f;   // resize handle hit zone (kResizeHandle)
        constexpr float kTB = 28.0f;  // title bar height (kTitleBarHeight)
        const auto& ws = workspaces_[active_workspace_];
        bool has_modal = std::any_of(ws.windows_.begin(), ws.windows_.end(),
            [](const Window& w) { return w.is_modal_; });
        for (auto it = ws.windows_.rbegin(); it != ws.windows_.rend(); ++it) {
            const auto& w = *it;
            if (w.is_shell_ || w.is_minimized_ || w.is_subsurface_ || w.layer_ >= 0) continue;
            if (has_modal && !w.is_modal_) continue;

            float wx = w.position_.x_;
            float wy = w.position_.y_;
            float ww = w.size_.width_;
            float wh = w.size_.height_;

            bool in_x = (mouse_pos.x_ >= wx - kRH &&
                         mouse_pos.x_ <= wx + ww + kRH);
            bool in_y = (mouse_pos.y_ >= wy - kTB &&
                         mouse_pos.y_ <= wy + wh + kRH);
            if (!in_x || !in_y) continue;

            bool on_left   = (mouse_pos.x_ >= wx - kRH &&
                              mouse_pos.x_ <  wx + kRH * 0.5f);
            bool on_right  = (mouse_pos.x_ >  wx + ww - kRH * 0.5f &&
                              mouse_pos.x_ <= wx + ww + kRH);
            bool on_bottom = (mouse_pos.y_ >  wy + wh - kRH * 0.5f &&
                              mouse_pos.y_ <= wy + wh + kRH);

            if (!on_left && !on_right && !on_bottom) continue;

            uint32_t edges = 0;
            if (on_left)   edges |= 4;
            if (on_right)  edges |= 8;
            if (on_bottom) edges |= 2;
            return std::pair{w.surface_id_, edges};
        }
        return std::nullopt;
    }

    void startResize(uint32_t surface_id, xkit::Offset cursor, uint32_t edges) noexcept {
        auto& ws = workspaces_[active_workspace_];
        for (auto& w : ws.windows_) {
            if (w.surface_id_ == surface_id && !w.is_shell_ && w.layer_ < 0) {
                w.is_resizing_          = true;
                w.resize_edges_         = edges;
                w.resize_cursor_start_  = cursor;
                w.resize_pos_start_     = w.position_;
                w.resize_size_start_    = w.size_;
                return;
            }
        }
    }

    // Returns {surface_id, new_size} while a resize is in progress.
    [[nodiscard]] std::optional<std::pair<uint32_t, xkit::Size>>
    updateResize(xkit::Offset cursor) noexcept {
        auto& ws = workspaces_[active_workspace_];
        for (auto& w : ws.windows_) {
            if (!w.is_resizing_) continue;

            float dx = cursor.x_ - w.resize_cursor_start_.x_;
            float dy = cursor.y_ - w.resize_cursor_start_.y_;

            float new_x = w.resize_pos_start_.x_;
            float new_w = w.resize_size_start_.width_;
            float new_h = w.resize_size_start_.height_;

            if (w.resize_edges_ & 4) {          // left edge
                new_w = w.resize_size_start_.width_ - dx;
                new_w = std::max(new_w, 120.0f);
                new_x = w.resize_pos_start_.x_ + w.resize_size_start_.width_ - new_w;
            } else if (w.resize_edges_ & 8) {   // right edge
                new_w = std::max(w.resize_size_start_.width_ + dx, 120.0f);
            }
            if (w.resize_edges_ & 2) {          // bottom edge
                new_h = std::max(w.resize_size_start_.height_ + dy, 80.0f);
            }

            w.target_position_ = {new_x, w.resize_pos_start_.y_};
            w.target_size_     = {new_w, new_h};

            return std::pair{w.surface_id_, xkit::Size{new_w, new_h}};
        }
        return std::nullopt;
    }

    void endResize() noexcept {
        for (auto& ws : workspaces_) {
            for (auto& w : ws.windows_) {
                if (w.is_resizing_)
                    w.position_ = w.target_position_; // commit left-edge position change
                w.is_resizing_ = false;
            }
        }
    }

    // Set or clear the xdg_toplevel parent relationship.
    // Centers child on parent and ensures child renders above parent.
    void setToplevelParent(uint32_t child_id, uint32_t parent_id) noexcept {
        bool found = false;
        for (auto& ws : workspaces_) {
            for (auto& w : ws.windows_) {
                if (w.surface_id_ == child_id) {
                    w.toplevel_parent_id_ = parent_id;
                    found = true;
                }
            }
        }
        if (!found) {
            // Window not yet added — store for application in addWindow
            if (parent_id != 0) pending_parents_[child_id] = parent_id;
            else                 pending_parents_.erase(child_id);
            return;
        }
        if (parent_id == 0) return;

        // Center child on parent and z-order child above parent in active workspace
        auto& wins = workspaces_[active_workspace_].windows_;
        for (auto& w : wins) {
            if (w.surface_id_ != parent_id) continue;
            float cx = w.position_.x_ + (w.size_.width_  - 0.0f) * 0.5f;
            float cy = w.position_.y_ + (w.size_.height_ - 0.0f) * 0.5f;
            for (auto& c : wins) {
                if (c.surface_id_ == child_id) {
                    cx -= c.size_.width_  * 0.5f;
                    cy -= c.size_.height_ * 0.5f;
                    c.position_        = {cx, cy};
                    c.target_position_ = {cx, cy};
                }
            }
            break;
        }
    }

    void removeWindow(uint32_t surface_id) noexcept {
        std::cout << "[window_stack] Removing Window ID: " << surface_id << "\n";
        for (auto& ws : workspaces_) {
            ws.windows_.erase(std::remove_if(ws.windows_.begin(), ws.windows_.end(),
                [surface_id](const Window& w) { return w.surface_id_ == surface_id; }),
                ws.windows_.end());
        }
        updateLayout();
    }

    void updateSurface(uint32_t surface_id, VkImage image, uint32_t width, uint32_t height,
                       float dx = 0, float dy = 0, float dw = 0, float dh = 0) noexcept {
        for (auto& ws : workspaces_) {
            for (auto& w : ws.windows_) {
                if (w.surface_id_ == surface_id) {
                    w.surface_image_ = image;
                    if (ws.layout_mode_ == LayoutMode::Floating) {
                        w.target_size_ = {(float)width, (float)height};
                    }
                    w.damage_pos_  = {dx, dy};
                    w.damage_size_ = {dw, dh};
                }
            }
        }
    }

    void clearDamage() noexcept {
        for (auto& ws : workspaces_) {
            for (auto& w : ws.windows_) w.damage_size_ = {0, 0};
        }
    }

    void setWorkspace(size_t index) noexcept {
        if (index < workspaces_.size()) {
            active_workspace_ = index;
            target_scene_offset_ = -static_cast<float>(index) * screen_w_;
        }
    }

    [[nodiscard]] size_t getActiveWorkspace() const noexcept { return active_workspace_; }

    void slideWorkspace(int direction) noexcept {
        int next = static_cast<int>(active_workspace_) + direction;
        if (next >= 0 && next < (int)workspaces_.size())
            setWorkspace(static_cast<size_t>(next));
    }

    void startDrag(uint32_t surface_id, xkit::Offset mouse_pos) noexcept {
        auto& ws = workspaces_[active_workspace_];
        for (auto& w : ws.windows_) {
            if (w.surface_id_ == surface_id) {
                w.is_dragging_  = true;
                w.drag_offset_  = {mouse_pos.x_ - w.position_.x_, mouse_pos.y_ - w.position_.y_};
                return;
            }
        }
    }

    // Track cursor position for output-aware window placement.
    void setCursorPosition(float x, float y) noexcept { cursor_x_ = x; cursor_y_ = y; }

    void updateDrag(xkit::Offset mouse_pos) noexcept {
        cursor_x_ = mouse_pos.x_; cursor_y_ = mouse_pos.y_;
        auto& ws = workspaces_[active_workspace_];
        for (auto& w : ws.windows_) {
            if (!w.is_dragging_) continue;
            w.target_position_ = {
                mouse_pos.x_ - w.drag_offset_.x_,
                mouse_pos.y_ - w.drag_offset_.y_
            };
            // Detect output migration: when the window centre crosses an output boundary.
            float cx = w.target_position_.x_ + w.size_.width_  * 0.5f;
            uint32_t new_oi = outputForX(cx);
            if (new_oi != w.output_idx_) {
                std::cout << "[wm] Window " << w.surface_id_
                          << " migrated to output " << new_oi << "\n";
                w.output_idx_ = new_oi;
            }
            return;
        }
    }

    void endDrag() noexcept {
        for (auto& ws : workspaces_) for (auto& w : ws.windows_) w.is_dragging_ = false;
    }

    void cycleLayoutMode() noexcept {
        auto& ws = workspaces_[active_workspace_];
        if (ws.layout_mode_ == LayoutMode::Floating)       ws.layout_mode_ = LayoutMode::Tiling;
        else if (ws.layout_mode_ == LayoutMode::Tiling)    ws.layout_mode_ = LayoutMode::MissionControl;
        else                                                ws.layout_mode_ = LayoutMode::Floating;
        updateLayout();
    }

    // Grow/shrink the master column in Tiling mode (Super+[ / Super+]). Clamped to
    // [0.1, 0.9]; re-tiles immediately.
    void adjustMasterRatio(float delta) noexcept {
        auto& ws = workspaces_[active_workspace_];
        ws.master_ratio_ = std::clamp(ws.master_ratio_ + delta, 0.1f, 0.9f);
        updateLayout();
    }
    [[nodiscard]] float masterRatio() const noexcept {
        return workspaces_[active_workspace_].master_ratio_;
    }

    // Returns the surface_id if the point is in a window's title bar (drag zone).
    [[nodiscard]] std::optional<uint32_t> hitTest(xkit::Offset mouse_pos) const noexcept {
        const auto& ws = workspaces_[active_workspace_];
        bool has_modal = std::any_of(ws.windows_.begin(), ws.windows_.end(),
            [](const Window& w) { return w.is_modal_; });
        for (auto it = ws.windows_.rbegin(); it != ws.windows_.rend(); ++it) {
            const auto& w = *it;
            if (w.is_shell_ || w.is_minimized_ || w.layer_ >= 0) continue;
            if (has_modal && !w.is_modal_) continue;
            if (mouse_pos.x_ >= w.position_.x_ &&
                mouse_pos.x_ <= w.position_.x_ + w.size_.width_ &&
                mouse_pos.y_ >= w.position_.y_ - 28.0f &&
                mouse_pos.y_ <= w.position_.y_) {
                return w.surface_id_;
            }
        }
        return std::nullopt;
    }

    // Returns the surface_id if the point lands anywhere in the window (title bar + body).
    [[nodiscard]] std::optional<uint32_t> bodyHitTest(xkit::Offset mouse_pos) const noexcept {
        const auto& ws = workspaces_[active_workspace_];
        bool has_modal = std::any_of(ws.windows_.begin(), ws.windows_.end(),
            [](const Window& w) { return w.is_modal_; });
        for (auto it = ws.windows_.rbegin(); it != ws.windows_.rend(); ++it) {
            const auto& w = *it;
            if (w.is_shell_ || w.is_minimized_ || w.layer_ >= 0) continue;
            if (has_modal && !w.is_modal_) continue;
            if (mouse_pos.x_ >= w.position_.x_ &&
                mouse_pos.x_ <= w.position_.x_ + w.size_.width_ &&
                mouse_pos.y_ >= w.position_.y_ - 28.0f &&
                mouse_pos.y_ <= w.position_.y_ + w.size_.height_) {
                return w.surface_id_;
            }
        }
        return std::nullopt;
    }

    // Move window to the top of the render stack (last drawn = on top).
    void raiseToFocus(uint32_t surface_id) noexcept {
        auto& wins = workspaces_[active_workspace_].windows_;
        auto it = std::find_if(wins.begin(), wins.end(),
            [surface_id](const Window& w) { return w.surface_id_ == surface_id; });
        if (it != wins.end() && !it->is_shell_ && it->layer_ < 0) {
            Window win = std::move(*it);
            wins.erase(it);
            wins.push_back(std::move(win));
        }
    }

    // Directional focus: return the visible window nearest to `current` in
    // direction `d` (window centres from their target rects), or `current` if
    // none. The caller updates its focus + calls raiseToFocus.
    [[nodiscard]] uint32_t focusDirection(uint32_t current, layout::Dir d) const noexcept {
        const auto& wins = workspaces_[active_workspace_].windows_;
        std::vector<layout::WinCenter> centers;
        for (const auto& w : wins) {
            if (w.is_shell_ || w.is_minimized_ || w.layer_ >= 0 || w.is_subsurface_) continue;
            centers.push_back({ w.surface_id_,
                                w.target_position_.x_ + w.target_size_.width_  * 0.5f,
                                w.target_position_.y_ + w.target_size_.height_ * 0.5f });
        }
        return layout::pickDirectional(centers, current, d);
    }

    // Promote a window to the tiling master slot (dwm-style "zoom"). Master is
    // the first tiled window in list order; swap the target into it. If the
    // target already is master, swap with the next tiled window so a repeat
    // press toggles. Re-tiles.
    void swapWithMaster(uint32_t surface_id) noexcept {
        auto& wins = workspaces_[active_workspace_].windows_;
        auto tiled = [](const Window& w) {
            return !w.is_shell_ && !w.is_minimized_ && w.layer_ < 0 && !w.is_subsurface_;
        };
        std::vector<size_t> idx;
        for (size_t i = 0; i < wins.size(); ++i) if (tiled(wins[i])) idx.push_back(i);
        if (idx.size() < 2) return;

        size_t master = idx[0];
        size_t target = master;
        for (size_t i : idx) if (wins[i].surface_id_ == surface_id) { target = i; break; }
        if (target == master) target = idx[1];   // focused is master → promote next
        std::swap(wins[master], wins[target]);
        updateLayout();
    }

    void minimizeWindow(uint32_t surface_id) noexcept {
        for (auto& w : workspaces_[active_workspace_].windows_) {
            if ((w.surface_id_ == surface_id || w.toplevel_parent_id_ == surface_id)
                    && !w.is_shell_ && w.layer_ < 0) {
                w.is_minimized_   = true;
                w.target_opacity_ = 0.0f;
                w.target_scale_   = 0.5f;
            }
        }
    }

    void maximizeWindow(uint32_t surface_id) noexcept {
        for (auto& w : workspaces_[active_workspace_].windows_) {
            if (w.surface_id_ == surface_id && !w.is_shell_ && w.layer_ < 0) {
                if (w.is_maximized_) {
                    w.target_position_ = w.restore_rect_.position_;
                    w.target_size_     = w.restore_rect_.size_;
                    w.is_maximized_    = false;
                } else {
                    w.restore_rect_ = {w.position_, w.size_};
                    // Maximize within the window's assigned output.
                    float x_off = 0.0f, out_w = screen_w_, out_h = screen_h_;
                    if (w.output_idx_ < outputs_.size()) {
                        x_off = outputs_[w.output_idx_].x_offset_;
                        out_w = outputs_[w.output_idx_].width_;
                        out_h = outputs_[w.output_idx_].height_;
                    }
                    w.target_position_ = {x_off, 28.0f};
                    w.target_size_     = {out_w, out_h - 28.0f};
                    w.is_maximized_    = true;
                }
            }
        }
    }

    // Snap a window to the left or right half of its output.
    // direction: -1 = left half, +1 = right half
    void snapWindow(uint32_t surface_id, int direction) noexcept {
        for (auto& w : workspaces_[active_workspace_].windows_) {
            if (w.surface_id_ != surface_id || w.is_shell_ || w.layer_ >= 0) continue;
            if (!w.is_maximized_)
                w.restore_rect_ = {w.position_, w.size_};
            float x_off = 0.0f, out_w = screen_w_, out_h = screen_h_;
            if (w.output_idx_ < outputs_.size()) {
                x_off = outputs_[w.output_idx_].x_offset_;
                out_w = outputs_[w.output_idx_].width_;
                out_h = outputs_[w.output_idx_].height_;
            }
            float half_w = out_w * 0.5f;
            w.target_position_ = (direction < 0)
                ? xkit::Offset{x_off,            28.0f}
                : xkit::Offset{x_off + half_w,   28.0f};
            w.target_size_     = {half_w, out_h - 28.0f};
            w.is_maximized_    = false;
            return;
        }
    }

    // Snap a window to one quarter of its output.
    // col: -1 = left half,  +1 = right half
    // row: -1 = top half,   +1 = bottom half
    void snapWindowQuarter(uint32_t surface_id, int col, int row) noexcept {
        for (auto& w : workspaces_[active_workspace_].windows_) {
            if (w.surface_id_ != surface_id || w.is_shell_ || w.layer_ >= 0) continue;
            if (!w.is_maximized_)
                w.restore_rect_ = {w.position_, w.size_};
            float x_off = 0.0f, out_w = screen_w_, out_h = screen_h_;
            if (w.output_idx_ < outputs_.size()) {
                x_off = outputs_[w.output_idx_].x_offset_;
                out_w = outputs_[w.output_idx_].width_;
                out_h = outputs_[w.output_idx_].height_;
            }
            float half_w  = out_w * 0.5f;
            float usable_h = out_h - 28.0f; // below taskbar
            float half_h  = usable_h * 0.5f;
            w.target_position_ = {
                (col < 0) ? x_off : x_off + half_w,
                (row < 0) ? 28.0f : 28.0f + half_h
            };
            w.target_size_  = {half_w, half_h};
            w.is_maximized_ = false;
            return;
        }
    }

    // Restore a window from maximised or snapped state to its pre-snap size.
    void restoreWindow(uint32_t surface_id) noexcept {
        for (auto& w : workspaces_[active_workspace_].windows_) {
            if (w.surface_id_ != surface_id || w.is_shell_ || w.layer_ >= 0) continue;
            if (w.restore_rect_.size_.width_ > 0.0f) {
                w.target_position_ = w.restore_rect_.position_;
                w.target_size_     = w.restore_rect_.size_;
            }
            w.is_maximized_ = false;
            return;
        }
    }

    // Move a window to a specific output (keyboard shortcut / API).
    void moveWindowToOutput(uint32_t surface_id, uint32_t output_idx) noexcept {
        if (output_idx >= outputs_.size()) return;
        for (auto& w : workspaces_[active_workspace_].windows_) {
            if (w.surface_id_ == surface_id && !w.is_shell_) {
                float x_off = outputs_[output_idx].x_offset_;
                w.output_idx_     = output_idx;
                w.target_position_ = {x_off + 50.0f, 78.0f};
                w.is_maximized_   = false;
            }
        }
        updateLayout();
    }

    [[nodiscard]] const std::vector<OutputInfo>& outputLayout() const noexcept { return outputs_; }

    // Returns the output index for a surface, searching all workspaces.
    [[nodiscard]] uint32_t getWindowOutput(uint32_t surface_id) const noexcept {
        for (const auto& ws : workspaces_) {
            for (const auto& w : ws.windows_) {
                if (w.surface_id_ == surface_id) return w.output_idx_;
            }
        }
        return 0;
    }

    void setWindowTitle(uint32_t surface_id, const std::string& title) noexcept {
        for (auto& ws : workspaces_) {
            for (auto& w : ws.windows_) {
                if (w.surface_id_ == surface_id) { w.title_ = title; return; }
            }
        }
    }

    // Apply committed xdg_surface.set_window_geometry. Called from xserver on FrameReady
    // when geom_w > 0. Propagates to all workspaces so the render offset is consistent.
    void setWindowGeometry(uint32_t surface_id, int32_t x, int32_t y, int32_t w, int32_t h) noexcept {
        for (auto& ws : workspaces_) {
            for (auto& win : ws.windows_) {
                if (win.surface_id_ == surface_id) {
                    win.geom_x_ = x; win.geom_y_ = y;
                    win.geom_w_ = w; win.geom_h_ = h;
                }
            }
        }
    }

    float scene_offset_{0.0f};
    float target_scene_offset_{0.0f};
    float scene_velocity_{0.0f};

private:
    // Returns the index of the output that contains global x coordinate.
    [[nodiscard]] uint32_t outputForX(float x) const noexcept {
        for (uint32_t i = 0; i < outputs_.size(); ++i) {
            if (x >= outputs_[i].x_offset_ &&
                x <  outputs_[i].x_offset_ + outputs_[i].width_)
                return i;
        }
        // Clamp to last output if beyond the right edge
        return outputs_.empty() ? 0u : static_cast<uint32_t>(outputs_.size() - 1u);
    }

    void updateLayout() noexcept {
        workspaces_[active_workspace_].applyTiling(outputs_);
    }

    std::vector<Workspace> workspaces_;
    size_t active_workspace_{0};
    float screen_w_{1920}, screen_h_{1080};
    float cursor_x_{960}, cursor_y_{540};

    std::vector<OutputInfo> outputs_;
    std::unordered_map<uint32_t, uint32_t> pending_parents_; // child_id → parent_id before window is added
};

} // namespace fbx::xserver
