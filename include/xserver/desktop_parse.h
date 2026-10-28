#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Pure helpers for com.freebsdx.AppPerms (bundle manifests),
// com.freebsdx.GameController (`sysctl kern.evdev.input`) and
// com.freebsdx.Backup (job list). Dependency-free / unit-testable; no syscalls.
namespace fbx::xserver::desktop {

namespace detail {
inline std::vector<std::string_view> lines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t nl = s.find('\n', start);
        if (nl == std::string_view::npos) nl = s.size();
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}
inline std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}
inline uint32_t fnv(std::string_view s) {
    uint32_t h = 2166136261u;
    for (char c : s) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
    return h ? h : 1u;
}
}  // namespace detail

// ── Bundle manifests ─────────────────────────────────────────────────────────

// The string value of top-level `"key": "value"` in a manifest.json, or "".
// Handles \" and \\ escapes; enough for the flat manifests we generate.
inline std::string jsonString(std::string_view json, std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t k = json.find(needle);
    if (k == std::string_view::npos) return {};
    size_t i = json.find(':', k + needle.size());
    if (i == std::string_view::npos) return {};
    i = json.find('"', i + 1);
    if (i == std::string_view::npos) return {};
    std::string out;
    for (++i; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) ++i;
        out += json[i];
    }
    return out;
}

// ── Game controllers (evdev) ─────────────────────────────────────────────────

struct EvdevDevice {
    uint32_t    index = 0;     // kern.evdev.input.<index>
    std::string name, phys, uniq;
    uint32_t    bustype = 0, vendor = 0, product = 0;
};

// `sysctl kern.evdev.input` → one entry per index, in index order.
inline std::vector<EvdevDevice> parseEvdevSysctl(std::string_view out) {
    std::map<uint32_t, EvdevDevice> by_index;
    constexpr std::string_view kPrefix = "kern.evdev.input.";
    for (auto line : detail::lines(out)) {
        if (line.substr(0, kPrefix.size()) != kPrefix) continue;
        line.remove_prefix(kPrefix.size());
        size_t dot = line.find('.'), colon = line.find(": ");
        if (dot == std::string_view::npos || colon == std::string_view::npos || dot > colon) continue;
        uint32_t idx = 0;
        bool num = dot > 0;
        for (char c : line.substr(0, dot)) {
            if (c < '0' || c > '9') { num = false; break; }
            idx = idx * 10 + static_cast<uint32_t>(c - '0');
        }
        if (!num) continue;
        std::string_view key = line.substr(dot + 1, colon - dot - 1), val = line.substr(colon + 2);
        auto& d = by_index[idx];
        d.index = idx;
        if (key == "name") d.name = std::string(val);
        else if (key == "phys") d.phys = std::string(val);
        else if (key == "uniq") d.uniq = std::string(val);
        else if (key == "id") {
            auto hexAfter = [&](std::string_view field) -> uint32_t {
                size_t p = val.find(field);
                if (p == std::string_view::npos) return 0;
                unsigned v = 0;
                std::sscanf(std::string(val.substr(p + field.size())).c_str(), " = %x", &v);
                return v;
            };
            d.bustype = hexAfter("bustype");
            d.vendor  = hexAfter("vendor");
            d.product = hexAfter("product");
        }
    }
    std::vector<EvdevDevice> v;
    for (auto& [i, d] : by_index) v.push_back(std::move(d));
    return v;
}

constexpr uint32_t kBusUsb = 0x03, kBusBluetooth = 0x05;

// A game controller, by name — evdev doesn't say "gamepad" in the sysctls.
// Sony/Nintendo/Valve devices count unless they name themselves a keyboard,
// mouse or touchpad (their touchpad/motion sub-devices).
inline bool isGameController(const EvdevDevice& d) {
    std::string n = detail::lower(d.name);
    for (std::string_view no : {"keyboard", "mouse", "touchpad", "motion sensors", "lid switch", "button"})
        if (n.find(no) != std::string::npos) return false;
    for (std::string_view yes : {"gamepad", "joystick", "game controller", "wireless controller",
                                 "pro controller", "xbox", "x-box", "dualshock", "dualsense", "8bitdo", "joy-con"})
        if (n.find(yes) != std::string::npos) return true;
    // "pad" as a word of its own ("X-Box 360 pad"), not inside "keypad".
    for (size_t p = n.find("pad"); p != std::string::npos; p = n.find("pad", p + 1)) {
        bool start = p == 0 || n[p - 1] == ' ';
        bool end = p + 3 == n.size() || n[p + 3] == ' ';
        if (start && end) return true;
    }
    return d.vendor == 0x054c || d.vendor == 0x057e || d.vendor == 0x28de;   // Sony, Nintendo, Valve
}

// Stable across replug/reboot: name + physical path (+ serial if any).
inline uint32_t controllerId(const EvdevDevice& d) {
    return detail::fnv(d.name + "|" + d.phys + "|" + d.uniq);
}

// ── Backup jobs ──────────────────────────────────────────────────────────────

struct BackupJob {
    std::string name;
    std::string source;        // directory to archive
    std::string destination;   // directory the .tar.gz goes into
};

// ~/.config/freebsdx/backups: "name<TAB>source<TAB>destination" per line;
// "~" expands to `home`. Without any valid line: the home folder → ~/Backups.
inline std::vector<BackupJob> parseBackupJobs(std::string_view text, const std::string& home) {
    auto expand = [&](std::string_view p) {
        if (p == "~") return home;
        if (p.substr(0, 2) == "~/") return home + std::string(p.substr(1));
        return std::string(p);
    };
    std::vector<BackupJob> jobs;
    for (auto line : detail::lines(text)) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line[0] == '#') continue;
        size_t t1 = line.find('\t');
        size_t t2 = t1 == std::string_view::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string_view::npos) continue;
        BackupJob j{std::string(line.substr(0, t1)), expand(line.substr(t1 + 1, t2 - t1 - 1)),
                    expand(line.substr(t2 + 1))};
        if (j.name.empty() || j.source.empty() || j.source[0] != '/' ||
            j.destination.empty() || j.destination[0] != '/')
            continue;
        jobs.push_back(std::move(j));
    }
    if (jobs.empty()) jobs.push_back({"Home folder", home, home + "/Backups"});
    return jobs;
}

inline std::string jobId(const BackupJob& j) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", detail::fnv(j.name));
    return buf;
}

// "Home folder" → "home-folder" (archive file prefix).
inline std::string slug(std::string_view name) {
    std::string s;
    for (char c : name) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) s += static_cast<char>(std::tolower(u));
        else if (!s.empty() && s.back() != '-') s += '-';
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    return s.empty() ? "backup" : s;
}

// Path of `inner` relative to `outer` if inner is inside outer, else "".
// Used to exclude the destination from its own archive.
inline std::string relativeInside(const std::string& outer, const std::string& inner) {
    if (inner.size() <= outer.size() + 1 || inner.compare(0, outer.size(), outer) != 0 ||
        inner[outer.size()] != '/')
        return {};
    return inner.substr(outer.size() + 1);
}

}  // namespace fbx::xserver::desktop
