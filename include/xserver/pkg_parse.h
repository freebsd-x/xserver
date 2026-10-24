#pragma once

#include <string>
#include <string_view>
#include <vector>

// Pure parser for `pkg upgrade -n` output, shared by com.freebsdx.Packages and
// com.freebsdx.PkgUpdates. Extracts the "name: old -> new" upgrade lines.
// Dependency-free / unit-testable (running pkg lives in the services).
namespace fbx::xserver::pkg {

struct Upgrade {
    std::string name;
    std::string old_ver;
    std::string new_ver;
};

namespace detail {
inline std::string_view trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}
inline std::string_view firstToken(std::string_view s) {
    size_t e = 0;
    while (e < s.size() && s[e] != ' ' && s[e] != '\t' && s[e] != '[') ++e;
    return s.substr(0, e);
}
}  // namespace detail

// Parse one upgrade line ("\tcurl: 8.1.0 -> 8.2.0" possibly with a trailing
// annotation). Returns false if the line isn't an upgrade line.
inline bool parseLine(std::string_view line, Upgrade& out) noexcept {
    size_t arrow = line.find(" -> ");
    if (arrow == std::string_view::npos) return false;
    size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon > arrow) return false;

    auto name = detail::trim(line.substr(0, colon));
    auto oldv = detail::trim(line.substr(colon + 1, arrow - (colon + 1)));
    auto newv = detail::firstToken(detail::trim(line.substr(arrow + 4)));
    if (name.empty() || oldv.empty() || newv.empty()) return false;
    out = {std::string(name), std::string(oldv), std::string(newv)};
    return true;
}

inline std::vector<Upgrade> parseUpgrades(std::string_view text) noexcept {
    std::vector<Upgrade> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos
                                                     ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
        Upgrade u;
        if (parseLine(line, u)) out.push_back(std::move(u));
    }
    return out;
}

}  // namespace fbx::xserver::pkg
