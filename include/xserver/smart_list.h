#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cctype>

// Pure parser for SMART disk health (com.freebsdx.Smart). Two inputs:
//   • `sysctl -n kern.disks`  → the SMART-capable disk list (ada/da/nvd/…)
//   • `smartctl -H -A /dev/X` → health + the ATA attribute table (or NVMe lines)
// Dependency-free / unit-testable; the service runs the tools via posix_spawn.
// Degrades cleanly: no smartctl / no permission → health "N/A", values unknown.
namespace fbx::xserver::smart {

struct SmartInfo {
    std::string name;                     // "ada0"
    std::string health = "N/A";           // PASSED / FAILED / CAUTION / N/A
    int32_t     temp_c = INT32_MIN;       // INT32_MIN = unknown
    uint32_t    reallocated = 0;
    uint32_t    power_on_hours = 0;
};

namespace detail {
inline void forEachLine(std::string_view text, auto&& fn) {
    size_t start = 0;
    while (true) {
        size_t nl = text.find('\n', start);
        fn(text.substr(start, (nl == std::string_view::npos ? text.size() : nl) - start));
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
}
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
// First unsigned integer in a string, skipping any leading non-digits
// ("41"→41, "  40 Celsius"→40, "1,234"→1234, "41 (Min/Max…)"→41); 0 if none.
inline uint32_t leadingUint(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && !(s[i] >= '0' && s[i] <= '9')) ++i;   // skip to first digit
    uint32_t v = 0; bool any = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') { v = v * 10 + static_cast<uint32_t>(c - '0'); any = true; }
        else if (c == ',') continue;        // 12,345 → 12345
        else break;
    }
    return any ? v : 0;
}
}  // namespace detail

// `sysctl -n kern.disks` ("ada0 da0 cd0 nvd0") → SMART-capable disks only.
inline std::vector<std::string> parseDiskList(std::string_view out) {
    std::vector<std::string> disks;
    for (auto& tok : detail::tokens(out)) {
        std::string_view n = tok;
        auto pfx = [&](const char* p) { return n.substr(0, std::string_view(p).size()) == p; };
        // ada*, da*, nda*/nvd*/nvme* (NVMe), vtbd*, mmcsd* are disks; cd/fd/pass are not.
        bool disk = pfx("ada") || pfx("nda") || pfx("nvd") || pfx("nvme") ||
                    pfx("vtbd") || pfx("mmcsd") ||
                    (pfx("da") && !pfx("ada") && !pfx("nda"));
        if (disk) disks.emplace_back(tok);
    }
    return disks;
}

// Merge a `smartctl -H -A` dump into an existing SmartInfo.
inline void parseSmartctl(std::string_view out, SmartInfo& d) {
    detail::forEachLine(out, [&](std::string_view line) {
        if (line.find("overall-health") != std::string_view::npos ||
            line.find("SMART Health Status") != std::string_view::npos) {
            auto t = detail::tokens(line);
            if (!t.empty()) {
                std::string h = t.back();
                if (h == "OK") h = "PASSED";
                d.health = h.substr(0, 7);
            }
            return;
        }
        // NVMe key:value lines.
        if (line.find("Temperature:") != std::string_view::npos) {
            auto c = line.find(':');
            d.temp_c = static_cast<int32_t>(detail::leadingUint(line.substr(c + 1)));
            return;
        }
        if (line.find("Power On Hours") != std::string_view::npos) {
            auto c = line.find(':');
            if (c != std::string_view::npos) d.power_on_hours = detail::leadingUint(line.substr(c + 1));
            return;
        }
        // ATA attribute table: "ID NAME FLAG VALUE WORST THRESH TYPE UPDATED WHEN RAW".
        auto t = detail::tokens(line);
        if (t.size() >= 10 && detail::isAllDigits(t[0])) {
            uint32_t id  = detail::leadingUint(t[0]);
            uint32_t raw = detail::leadingUint(t[9]);
            if      (id == 5)                 d.reallocated    = raw;
            else if (id == 9)                 d.power_on_hours = raw;
            else if (id == 194 || id == 190)  d.temp_c         = static_cast<int32_t>(raw);
        }
    });
}

}  // namespace fbx::xserver::smart
