#pragma once

#include "shared/protocol.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

// Where the shell's open popups are (start menu, search, dialogs, the Screen
// Time card…). The shell is one transparent full-screen surface above the app
// windows; without this a click on a popup that sits over an app window went to
// the app underneath. The shell reports its popups' rectangles
// (ShellOpcode::SetInputRegion) whenever they change, and the compositor routes
// pointer input inside them to the shell. Header-only / unit-testable.
namespace fbx::xserver::shellregion {

struct Rect { float x, y, w, h; };

inline std::mutex        g_mu;
inline std::vector<Rect> g_rects;

inline void set(const shared::ShellInputRegion& r) {
    std::vector<Rect> rects;
    const uint32_t n = std::min<uint32_t>(r.count_, std::size(r.rects_));
    for (uint32_t i = 0; i < n; ++i) {
        const auto& s = r.rects_[i];
        if (s.w_ > 0 && s.h_ > 0) rects.push_back({float(s.x_), float(s.y_), float(s.w_), float(s.h_)});
    }
    std::lock_guard lk(g_mu);
    g_rects = std::move(rects);
}

inline bool contains(float x, float y) {
    std::lock_guard lk(g_mu);
    return std::any_of(g_rects.begin(), g_rects.end(), [&](const Rect& r) {
        return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
    });
}

}  // namespace fbx::xserver::shellregion
