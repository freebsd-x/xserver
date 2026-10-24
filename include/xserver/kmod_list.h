#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cctype>

// Pure parser for kldstat(8) output (com.freebsdx.KernelModules). kldstat is
// base-system and readable unprivileged. Dependency-free / unit-testable; the
// service runs kldstat via posix_spawn and feeds the text here.
//
//   Id Refs Address                 Size Name
//    1   33 0xffffffff80200000  1f73478 kernel
//    2    1 0xffffffff82174000     3f90 ichsmb.ko
namespace fbx::xserver::kmod {

struct KmodInfo {
    std::string name;
    uint32_t    size_kb = 0;
    uint32_t    refs = 0;
};

namespace detail {
inline std::vector<std::string> tokens(std::string_view line) {
    std::vector<std::string> t;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        size_t b = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i > b) t.emplace_back(line.substr(b, i - b));
    }
    return t;
}
inline bool isAllDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}
// Hex integer, optional "0x" prefix (kldstat's Size column is bare hex).
inline uint32_t parseHex(std::string_view s) {
    size_t i = (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 2 : 0;
    uint32_t v = 0;
    for (; i < s.size(); ++i) {
        char c = s[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = v * 16 + static_cast<uint32_t>(d);
    }
    return v;
}
}  // namespace detail

inline std::vector<KmodInfo> parseKldstat(std::string_view out) {
    std::vector<KmodInfo> mods;
    size_t start = 0;
    while (true) {
        size_t nl = out.find('\n', start);
        std::string_view line = out.substr(start, (nl == std::string_view::npos ? out.size() : nl) - start);
        auto t = detail::tokens(line);
        // Data line: Id Refs Address Size Name — id is numeric, address is "0x…".
        if (t.size() >= 5 && detail::isAllDigits(t[0]) &&
            t[2].size() >= 2 && t[2][0] == '0' && (t[2][1] == 'x' || t[2][1] == 'X')) {
            KmodInfo m;
            for (char c : t[1])                       // Refs column is decimal
                if (c >= '0' && c <= '9') m.refs = m.refs * 10 + static_cast<uint32_t>(c - '0');
            m.size_kb = detail::parseHex(t[3]) / 1024u;  // Size column is bare hex bytes
            m.name    = t[4];
            mods.push_back(std::move(m));
        }
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return mods;
}

}  // namespace fbx::xserver::kmod
