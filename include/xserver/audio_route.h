#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Pure helpers for com.freebsdx.AudioRouting: the playback devices from
// /dev/sndstat and the per-app route table AudioService mixes by.
// Dependency-free / unit-testable.
namespace fbx::xserver::audioroute {

struct Output {
    uint32_t    unit = 0;          // pcmN → /dev/dspN
    std::string name;              // "Realtek ALC294 (Analog 2.0+HP/4ch)"
    bool        is_default = false;
    [[nodiscard]] std::string path() const { return "/dev/dsp" + std::to_string(unit); }
};

// `pcm1: <Intel Kaby Lake (HDMI/DP 8ch)> (play) default` → playback devices only.
inline std::vector<Output> parseSndstat(std::string_view text) {
    std::vector<Output> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(start, nl - start);
        start = nl + 1;
        if (line.substr(0, 3) != "pcm") continue;
        size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 3) continue;
        uint32_t unit = 0;
        bool num = true;
        for (char c : line.substr(3, colon - 3)) {
            if (c < '0' || c > '9') { num = false; break; }
            unit = unit * 10 + static_cast<uint32_t>(c - '0');
        }
        size_t lt = line.find('<', colon), gt = line.rfind('>');
        if (!num || lt == std::string_view::npos || gt == std::string_view::npos || gt < lt) continue;
        std::string_view rest = line.substr(gt + 1);
        if (rest.find("play") == std::string_view::npos) continue;   // capture-only
        out.push_back({unit, std::string(line.substr(lt + 1, gt - lt - 1)),
                       rest.find("default") != std::string_view::npos});
    }
    return out;
}

// A short label that fits AudioRouteEntry::device_name_[32]: the device's
// connector in parentheses wins ("Analog", "HDMI/DP"), else the chip name.
inline std::string shortLabel(const Output& o) {
    size_t lp = o.name.rfind('('), rp = o.name.rfind(')');
    if (lp != std::string::npos && rp != std::string::npos && rp > lp + 1) {
        std::string inner = o.name.substr(lp + 1, rp - lp - 1);
        size_t cut = inner.find_first_of(" +");
        std::string kind = inner.substr(0, cut);
        if (!kind.empty()) return kind + " (pcm" + std::to_string(o.unit) + ")";
    }
    return o.name.substr(0, 24);
}

// An open audio stream, as AudioService reports it.
struct Stream {
    uint32_t    id = 0;
    std::string app;
    std::string route;   // /dev/dspN, or "" = the default sink
};

// Which device does `app` play on? Its saved route if that device still
// exists, else "" (= the default sink).
inline std::string resolve(const std::map<std::string, std::string>& routes, const std::string& app,
                           const std::vector<Output>& outputs) {
    auto it = routes.find(app);
    if (it == routes.end()) return {};
    for (const auto& o : outputs)
        if (o.path() == it->second) return it->second;
    return {};
}

// Persisted as "app=/dev/dspN;app=/dev/dspN".
inline std::map<std::string, std::string> parseRoutes(std::string_view s) {
    std::map<std::string, std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t semi = s.find(';', start);
        if (semi == std::string_view::npos) semi = s.size();
        std::string_view item = s.substr(start, semi - start);
        start = semi + 1;
        size_t eq = item.rfind('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        std::string_view path = item.substr(eq + 1);
        if (path.substr(0, 8) != "/dev/dsp" || path.size() < 9 || path.size() > 11) continue;
        bool digits = true;
        for (char c : path.substr(8)) digits = digits && c >= '0' && c <= '9';
        if (digits) out[std::string(item.substr(0, eq))] = std::string(path);
    }
    return out;
}
inline std::string formatRoutes(const std::map<std::string, std::string>& m) {
    std::string s;
    for (const auto& [app, path] : m) s += (s.empty() ? "" : ";") + app + "=" + path;
    return s;
}
inline bool validApp(std::string_view a) {
    if (a.empty() || a.size() > 63) return false;
    for (char c : a) if (c == ';' || c == '=' || c == '\n' || c == '\0') return false;
    return true;
}

}  // namespace fbx::xserver::audioroute
