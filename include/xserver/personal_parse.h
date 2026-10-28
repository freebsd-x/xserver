#pragma once

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Pure helpers for com.freebsdx.Reminder and com.freebsdx.License.
// Dependency-free / unit-testable; no syscalls.
namespace fbx::xserver::personal {

// ── Reminders ────────────────────────────────────────────────────────────────

struct Reminder {
    uint32_t    event_id = 0;
    std::string title;
    std::time_t start = 0;
    std::time_t due = 0;
};

constexpr std::time_t kOverdueWindow  = 24 * 3600;      // keep overdue ones a day
constexpr std::time_t kUpcomingWindow = 7 * 24 * 3600;  // show a week ahead

// What the list shows at `now`: not done, due within the next week, and not
// more than a day overdue; soonest first.
inline std::vector<Reminder> selectReminders(std::vector<Reminder> all, std::time_t now,
                                             const std::set<uint32_t>& done) {
    std::vector<Reminder> out;
    for (auto& r : all)
        if (!done.count(r.event_id) && r.due <= now + kUpcomingWindow && r.due >= now - kOverdueWindow)
            out.push_back(std::move(r));
    std::stable_sort(out.begin(), out.end(),
                     [](const Reminder& a, const Reminder& b) { return a.due < b.due; });
    return out;
}

// "ev-<id>" on the wire ↔ the calendar event id.
inline std::string reminderWireId(uint32_t event_id) { return "ev-" + std::to_string(event_id); }
inline bool parseReminderWireId(std::string_view s, uint32_t& id) {
    if (s.substr(0, 3) != "ev-" || s.size() < 4 || s.size() > 13) return false;
    uint64_t v = 0;
    for (char c : s.substr(3)) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    if (v == 0 || v > 0xffffffffu) return false;
    id = static_cast<uint32_t>(v);
    return true;
}

// Done set persisted as "1;7;42".
inline std::set<uint32_t> parseIdSet(std::string_view s) {
    std::set<uint32_t> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t semi = s.find(';', start);
        if (semi == std::string_view::npos) semi = s.size();
        uint32_t id;
        if (parseReminderWireId("ev-" + std::string(s.substr(start, semi - start)), id)) out.insert(id);
        start = semi + 1;
    }
    return out;
}
inline std::string formatIdSet(const std::set<uint32_t>& ids) {
    std::string s;
    for (uint32_t id : ids) s += (s.empty() ? "" : ";") + std::to_string(id);
    return s;
}

// ── License ──────────────────────────────────────────────────────────────────

// key=value lines; '#' comments; whitespace around both trimmed.
inline std::map<std::string, std::string> parseKeyValues(std::string_view text) {
    std::map<std::string, std::string> out;
    auto trim = [](std::string_view v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.remove_suffix(1);
        return v;
    };
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = trim(text.substr(start, nl - start));
        start = nl + 1;
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        out[std::string(trim(line.substr(0, eq)))] = std::string(trim(line.substr(eq + 1)));
    }
    return out;
}

// Only the last 4 characters survive: "****-****-****-A3F7" (ASCII, so it fits
// LicenseState::license_key_[32]). Keys of 4 characters or fewer show none.
inline std::string maskKey(std::string_view key) {
    if (key.empty()) return {};
    std::string_view tail = key.size() > 4 ? key.substr(key.size() - 4) : std::string_view{};
    return "****-****-****-" + std::string(tail);
}

}  // namespace fbx::xserver::personal
