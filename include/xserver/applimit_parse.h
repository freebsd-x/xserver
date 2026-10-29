#pragma once

#include <cstdint>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <string_view>

// Pure helpers for com.freebsdx.AppTimeLimit: per-app daily limits, today's
// foreground usage, and when a limit has just been crossed.
// Dependency-free / unit-testable.
namespace fbx::xserver::applimit {

// Local calendar day, "2026-10-29" — usage resets when it changes.
inline std::string dayKey(std::time_t t) {
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

// "id=value;id=value" ↔ map. Ids are bundle identifiers (no ';' or '=').
inline std::map<std::string, uint32_t> parseMap(std::string_view s) {
    std::map<std::string, uint32_t> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t semi = s.find(';', start);
        if (semi == std::string_view::npos) semi = s.size();
        std::string_view item = s.substr(start, semi - start);
        start = semi + 1;
        size_t eq = item.rfind('=');
        if (eq == std::string_view::npos || eq == 0 || eq + 1 == item.size()) continue;
        uint64_t v = 0;
        bool ok = true;
        for (char c : item.substr(eq + 1)) {
            if (c < '0' || c > '9') { ok = false; break; }
            v = v * 10 + static_cast<uint64_t>(c - '0');
            if (v > 0xffffffffu) { ok = false; break; }
        }
        if (ok) out[std::string(item.substr(0, eq))] = static_cast<uint32_t>(v);
    }
    return out;
}
inline std::string formatMap(const std::map<std::string, uint32_t>& m) {
    std::string s;
    for (const auto& [k, v] : m) s += (s.empty() ? "" : ";") + k + "=" + std::to_string(v);
    return s;
}

// Today's usage: "<day>|id=secs;…". A different day → empty (it reset).
inline std::map<std::string, uint32_t> parseUsage(std::string_view s, std::string_view today) {
    size_t bar = s.find('|');
    if (bar == std::string_view::npos || s.substr(0, bar) != today) return {};
    return parseMap(s.substr(bar + 1));
}
inline std::string formatUsage(std::string_view today, const std::map<std::string, uint32_t>& m) {
    return std::string(today) + "|" + formatMap(m);
}

// Ids already told about today: "<day>|id;id".
inline std::set<std::string> parseNotified(std::string_view s, std::string_view today) {
    std::set<std::string> out;
    size_t bar = s.find('|');
    if (bar == std::string_view::npos || s.substr(0, bar) != today) return out;
    std::string_view rest = s.substr(bar + 1);
    size_t start = 0;
    while (start < rest.size()) {
        size_t semi = rest.find(';', start);
        if (semi == std::string_view::npos) semi = rest.size();
        if (semi > start) out.emplace(rest.substr(start, semi - start));
        start = semi + 1;
    }
    return out;
}
inline std::string formatNotified(std::string_view today, const std::set<std::string>& ids) {
    std::string s(today);
    s += '|';
    bool first = true;
    for (const auto& id : ids) { if (!first) s += ';'; s += id; first = false; }
    return s;
}

// Did adding time take usage from below the limit to at/over it?
inline bool crossed(uint32_t before_secs, uint32_t after_secs, uint32_t limit_min) {
    if (limit_min == 0) return false;
    const uint64_t limit = static_cast<uint64_t>(limit_min) * 60;
    return before_secs < limit && after_secs >= limit;
}

}  // namespace fbx::xserver::applimit
