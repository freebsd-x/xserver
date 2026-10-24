#pragma once

#include <string_view>

// Pure parser for backlight(8) output. Dependency-free / unit-testable; the
// service runs `backlight` via posix_spawn and feeds the text here.
//
//   $ backlight
//   brightness: 100
namespace fbx::xserver::backlight {

// Parse the "brightness: N" line → 0..100, or -1 if absent/unparseable.
inline int parseBrightness(std::string_view out) {
    constexpr std::string_view kKey = "brightness:";
    size_t p = out.find(kKey);
    if (p == std::string_view::npos) return -1;
    p += kKey.size();
    while (p < out.size() && (out[p] == ' ' || out[p] == '\t')) ++p;
    if (p >= out.size() || out[p] < '0' || out[p] > '9') return -1;
    int v = 0;
    while (p < out.size() && out[p] >= '0' && out[p] <= '9') { v = v * 10 + (out[p] - '0'); ++p; }
    return v > 100 ? 100 : v;
}

}  // namespace fbx::xserver::backlight
