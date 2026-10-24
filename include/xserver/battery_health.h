#pragma once

#include <string>
#include <string_view>
#include <cstdint>

// Pure parser for `acpiconf -i 0` (com.freebsdx.BatteryHealth). acpiconf is
// base-system and readable unprivileged on a laptop. Dependency-free /
// unit-testable; the service runs acpiconf via posix_spawn.
//
//   Design capacity:     50086 mWh
//   Last full capacity:  39153 mWh
//   Cycle Count:         21
namespace fbx::xserver::battery {

struct BatteryHealth {
    uint32_t design_mwh    = 0;
    uint32_t last_full_mwh = 0;
    uint32_t cycle_count   = 0;
};

namespace detail {
// First unsigned integer in a string, skipping leading non-digits; 0 if none.
inline uint32_t firstUint(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && !(s[i] >= '0' && s[i] <= '9')) ++i;
    uint32_t v = 0; bool any = false;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        v = v * 10 + static_cast<uint32_t>(s[i] - '0'); any = true;
    }
    return any ? v : 0;
}
}  // namespace detail

inline BatteryHealth parseAcpiconf(std::string_view out) {
    BatteryHealth b;
    size_t start = 0;
    while (true) {
        size_t nl = out.find('\n', start);
        std::string_view line = out.substr(start, (nl == std::string_view::npos ? out.size() : nl) - start);
        size_t colon = line.find(':');
        std::string_view val = (colon == std::string_view::npos) ? std::string_view{}
                                                                 : line.substr(colon + 1);
        if      (line.find("Design capacity")    != std::string_view::npos) b.design_mwh    = detail::firstUint(val);
        else if (line.find("Last full capacity") != std::string_view::npos) b.last_full_mwh = detail::firstUint(val);
        else if (line.find("Cycle Count")        != std::string_view::npos) b.cycle_count   = detail::firstUint(val);
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return b;
}

// Battery wear health: last-full capacity as a percentage of design capacity,
// clamped to 100. 0 when design capacity is unknown (no battery).
inline uint32_t healthPct(const BatteryHealth& b) {
    if (b.design_mwh == 0) return 0;
    uint64_t pct = static_cast<uint64_t>(b.last_full_mwh) * 100u / b.design_mwh;
    return pct > 100 ? 100u : static_cast<uint32_t>(pct);
}

}  // namespace fbx::xserver::battery
