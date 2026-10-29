#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <string_view>

// `geom disk list` → "nda0 (238G SAMSUNG MZVLW256HEHP-00000),ada0 (…)" for
// com.freebsdx.Storage GetDisks. Dependency-free / unit-testable.
namespace fbx::xserver::geom {

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

inline std::string diskSummary(std::string_view out) {
    std::string result, name, size, descr;
    auto flush = [&] {
        if (name.empty()) return;
        if (!result.empty()) result += ",";
        result += name + " (" + size + (descr.empty() || size.empty() ? "" : " ") + descr + ")";
        name.clear(); size.clear(); descr.clear();
    };
    size_t start = 0;
    while (start < out.size()) {
        size_t nl = out.find('\n', start);
        if (nl == std::string_view::npos) nl = out.size();
        std::string_view line = trim(out.substr(start, nl - start));
        start = nl + 1;
        if (line.substr(0, 10) == "Geom name:") { flush(); name = std::string(trim(line.substr(10))); }
        else if (line.substr(0, 10) == "Mediasize:") {
            // "Mediasize: 256060514304 (238G)" → "238G"
            size_t lp = line.find('('), rp = line.find(')');
            if (lp != std::string_view::npos && rp > lp) size = std::string(line.substr(lp + 1, rp - lp - 1));
        } else if (line.substr(0, 6) == "descr:") descr = std::string(trim(line.substr(6)));
    }
    flush();
    return result;
}

struct Disk {
    std::string name, descr, ident;   // ident = serial number
    uint64_t    bytes = 0;
};

// `geom disk list` → one Disk per "Geom name:" block.
inline std::vector<Disk> parseDisks(std::string_view out) {
    std::vector<Disk> disks;
    size_t start = 0;
    while (start < out.size()) {
        size_t nl = out.find('\n', start);
        if (nl == std::string_view::npos) nl = out.size();
        std::string_view line = trim(out.substr(start, nl - start));
        start = nl + 1;
        if (line.substr(0, 10) == "Geom name:") { disks.push_back({}); disks.back().name = std::string(trim(line.substr(10))); }
        else if (disks.empty()) continue;
        else if (line.substr(0, 10) == "Mediasize:")
            disks.back().bytes = std::strtoull(std::string(trim(line.substr(10))).c_str(), nullptr, 10);
        else if (line.substr(0, 6) == "descr:") disks.back().descr = std::string(trim(line.substr(6)));
        else if (line.substr(0, 6) == "ident:") disks.back().ident = std::string(trim(line.substr(6)));
    }
    return disks;
}

}  // namespace fbx::xserver::geom
