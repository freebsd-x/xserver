#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Pure helpers for notification quieting (com.freebsdx.NotifSchedule + the
// DoNotDisturb toggle) and per-app notification sounds (com.freebsdx.NotifSound).
// Dependency-free / unit-testable; no syscalls.
namespace fbx::xserver::notif {

// ── Quiet hours ──────────────────────────────────────────────────────────────

struct Schedule {
    bool     enabled = false;
    uint32_t start_min = 22 * 60;   // minutes after local midnight
    uint32_t end_min   = 7 * 60;
    bool     allow_calls  = true;
    bool     allow_alarms = true;
    bool     weekend_only = false;
};

// Is local time `t` inside the window? A window may cross midnight (22:00 →
// 07:00); start == end means no window. With weekend_only, a window counts
// only when it *started* on Saturday or Sunday (so Sunday 22:00 → Monday 07:00
// still holds until Monday 07:00).
inline bool inQuietWindow(const Schedule& s, const std::tm& t) {
    if (!s.enabled || s.start_min == s.end_min) return false;
    uint32_t now = static_cast<uint32_t>(t.tm_hour * 60 + t.tm_min);
    bool overnight = s.start_min > s.end_min;
    bool in = overnight ? (now >= s.start_min || now < s.end_min)
                        : (now >= s.start_min && now < s.end_min);
    if (!in || !s.weekend_only) return in;
    // The day the window started: today, or yesterday for the after-midnight part.
    int start_wday = (overnight && now < s.end_min) ? (t.tm_wday + 6) % 7 : t.tm_wday;
    return start_wday == 0 || start_wday == 6;
}

// Apps whose notifications get through quiet time when allowed.
inline bool isAlarmApp(std::string_view app) { return app == "Clock" || app == "Alarm" || app == "Calendar"; }
inline bool isCallApp(std::string_view app)  { return app == "Phone" || app == "Calls"; }

inline bool breaksThrough(const Schedule& s, std::string_view app) {
    return (s.allow_alarms && isAlarmApp(app)) || (s.allow_calls && isCallApp(app));
}

// ── Per-app sounds ───────────────────────────────────────────────────────────

enum Sound : uint32_t { kDefault = 0, kChime = 1, kPing = 2, kSilent = 3 };

// An app name we'll keep in the map (persisted as "app=id;app=id").
inline bool validAppName(std::string_view a) {
    if (a.empty() || a.size() > 63) return false;
    for (char c : a)
        if (c == ';' || c == '=' || c == '\n' || c == '\0') return false;
    return true;
}

inline std::map<std::string, uint32_t> parseSoundMap(std::string_view s) {
    std::map<std::string, uint32_t> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t semi = s.find(';', start);
        if (semi == std::string_view::npos) semi = s.size();
        std::string_view item = s.substr(start, semi - start);
        start = semi + 1;
        size_t eq = item.rfind('=');
        if (eq == std::string_view::npos) continue;
        std::string_view app = item.substr(0, eq), id = item.substr(eq + 1);
        if (!validAppName(app) || id.size() != 1 || id[0] < '0' || id[0] > '3') continue;
        out[std::string(app)] = static_cast<uint32_t>(id[0] - '0');
    }
    return out;
}

inline std::string formatSoundMap(const std::map<std::string, uint32_t>& m) {
    std::string s;
    for (const auto& [app, id] : m) s += (s.empty() ? "" : ";") + app + "=" + std::to_string(id);
    return s;
}

// A tone: one or two notes, each with a short attack and exponential decay.
// 16-bit mono PCM at `rate`, scaled by volume (0-100). Silent → empty.
inline std::vector<int16_t> synthesize(uint32_t sound, uint32_t volume_pct, uint32_t rate = 48000) {
    struct Note { double hz; double start_s; double len_s; };
    std::vector<Note> notes;
    switch (sound) {
    case kChime: notes = {{880.0, 0.0, 0.18}, {1318.5, 0.12, 0.30}}; break;   // A5 → E6
    case kPing:  notes = {{1568.0, 0.0, 0.22}}; break;                         // G6
    case kSilent: return {};
    default:     notes = {{1046.5, 0.0, 0.20}}; break;                         // C6
    }
    double total = 0;
    for (const auto& n : notes) total = std::max(total, n.start_s + n.len_s);
    std::vector<int16_t> pcm(static_cast<size_t>(total * rate), 0);
    double amp = 0.35 * 32767.0 * std::min<uint32_t>(volume_pct, 100) / 100.0;
    for (const auto& n : notes) {
        size_t first = static_cast<size_t>(n.start_s * rate);
        size_t count = static_cast<size_t>(n.len_s * rate);
        for (size_t i = 0; i < count && first + i < pcm.size(); ++i) {
            double t = static_cast<double>(i) / rate;
            double env = std::min(1.0, t / 0.005) * std::exp(-t * 14.0);
            double v = pcm[first + i] + amp * env * std::sin(2.0 * M_PI * n.hz * t);
            pcm[first + i] = static_cast<int16_t>(std::max(-32767.0, std::min(32767.0, v)));
        }
    }
    return pcm;
}

}  // namespace fbx::xserver::notif
