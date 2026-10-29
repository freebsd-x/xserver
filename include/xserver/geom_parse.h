#pragma once

#include <string>
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

}  // namespace fbx::xserver::geom
