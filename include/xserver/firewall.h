#pragma once

#include <string_view>
#include <cstdint>

// Pure parser for pf(4) status via pfctl(8). Dependency-free / unit-testable;
// the service runs pfctl (reads need root → degrade to disabled/0 unprivileged)
// and the actual enable/disable goes through the privilege broker.
namespace fbx::xserver::firewall {

// `pfctl -s info` → is pf enabled? (the "Status: Enabled" line).
inline bool parseEnabled(std::string_view info) noexcept {
    return info.find("Status: Enabled") != std::string_view::npos;
}

// `pfctl -s rules` → count of non-blank rule lines.
inline uint32_t countRules(std::string_view rules) noexcept {
    uint32_t n = 0;
    size_t pos = 0;
    while (pos < rules.size()) {
        size_t nl = rules.find('\n', pos);
        std::string_view line = rules.substr(pos, (nl == std::string_view::npos ? rules.size() : nl) - pos);
        pos = (nl == std::string_view::npos) ? rules.size() : nl + 1;
        size_t b = 0;
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
        if (b < line.size()) ++n;   // non-blank
    }
    return n;
}

}  // namespace fbx::xserver::firewall
