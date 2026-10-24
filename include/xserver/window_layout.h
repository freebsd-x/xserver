#pragma once

// Pure tiling-layout geometry — no dependencies, no side effects, so it is
// unit-testable in isolation and reused by WindowStack::applyTiling.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbx::xserver::layout {

struct TileRect { float x, y, w, h; };

enum class Dir { Left, Right, Up, Down };

// One window's centre for directional focus.
struct WinCenter { uint32_t id; float cx, cy; };

// Pick the nearest window in direction `d` from the window with id `current`.
// Only windows strictly on that side of the current centre are candidates;
// among them the one with the smallest weighted distance (primary-axis gap plus
// twice the perpendicular offset, so well-aligned neighbours win) is chosen.
// Returns `current` unchanged if there is no candidate or `current` is absent.
inline uint32_t pickDirectional(const std::vector<WinCenter>& wins,
                                uint32_t current, Dir d) noexcept {
    const WinCenter* cur = nullptr;
    for (const auto& w : wins) if (w.id == current) { cur = &w; break; }
    if (!cur) return current;

    uint32_t best = current;
    float best_score = -1.0f;
    for (const auto& w : wins) {
        if (w.id == current) continue;
        const float dx = w.cx - cur->cx;
        const float dy = w.cy - cur->cy;
        float primary, perp;
        switch (d) {
            case Dir::Left:  if (dx >= 0) continue; primary = -dx; perp = dy < 0 ? -dy : dy; break;
            case Dir::Right: if (dx <= 0) continue; primary =  dx; perp = dy < 0 ? -dy : dy; break;
            case Dir::Up:    if (dy >= 0) continue; primary = -dy; perp = dx < 0 ? -dx : dx; break;
            case Dir::Down:  if (dy <= 0) continue; primary =  dy; perp = dx < 0 ? -dx : dx; break;
        }
        const float score = primary + 2.0f * perp;   // lower = better
        if (best_score < 0.0f || score < best_score) { best_score = score; best = w.id; }
    }
    return best;
}

// Master/stack tiling (dwm/xmonad style): `master_count` windows fill a master
// column of width `output_w * master_ratio` on the left, split evenly by height;
// the remaining windows fill a stack column on the right, split evenly by height.
// Falls back gracefully:
//   - n == 0                → empty
//   - n <= master_count     → a single full-width column of n rows
// master_ratio is clamped to [0.1, 0.9]; master_count to >= 1.
// (ox, oy, ow, oh) is the usable output rect in logical coordinates.
inline std::vector<TileRect> masterStack(std::size_t n,
                                         float ox, float oy, float ow, float oh,
                                         float master_ratio = 0.6f,
                                         std::size_t master_count = 1) noexcept {
    std::vector<TileRect> out;
    if (n == 0 || ow <= 0.0f || oh <= 0.0f) return out;
    out.reserve(n);

    if (master_ratio < 0.1f) master_ratio = 0.1f;
    if (master_ratio > 0.9f) master_ratio = 0.9f;
    if (master_count < 1) master_count = 1;

    // Everything fits in the master column → one full-width column.
    if (n <= master_count) {
        const float rh = oh / static_cast<float>(n);
        for (std::size_t i = 0; i < n; ++i)
            out.push_back({ox, oy + rh * static_cast<float>(i), ow, rh});
        return out;
    }

    const std::size_t masters = master_count;
    const std::size_t stacked = n - masters;
    const float master_w = ow * master_ratio;
    const float stack_w  = ow - master_w;
    const float stack_x  = ox + master_w;

    const float mrh = oh / static_cast<float>(masters);
    for (std::size_t i = 0; i < masters; ++i)
        out.push_back({ox, oy + mrh * static_cast<float>(i), master_w, mrh});

    const float srh = oh / static_cast<float>(stacked);
    for (std::size_t i = 0; i < stacked; ++i)
        out.push_back({stack_x, oy + srh * static_cast<float>(i), stack_w, srh});

    return out;
}

}  // namespace fbx::xserver::layout
