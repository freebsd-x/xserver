#include "xserver/calendar_service.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <iostream>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

std::mutex         CalendarService::mutex_;
std::vector<CalendarService::Event> CalendarService::events_;
bool               CalendarService::loaded_ = false;
uint32_t           CalendarService::next_id_ = 1;

// ── Persistence ───────────────────────────────────────────────────────────────

std::string CalendarService::calendarPath() noexcept {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.freebsdx-calendar";
}

uint32_t CalendarService::nextId() noexcept {
    for (const auto& e : events_)
        if (e.id >= next_id_) next_id_ = e.id + 1;
    return next_id_++;
}

// Format: one event per line, pipe-separated
// id|year|month|day|hour|minute|end_hour|end_minute|reminder_min|all_day|title|description|location
static std::string escape(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '|') r += "\\|";
        else if (c == '\n') r += "\\n";
        else r += c;
    }
    return r;
}
static std::string unescape(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            if (s[i+1] == '|')  { r += '|'; ++i; }
            else if (s[i+1] == 'n') { r += '\n'; ++i; }
            else r += s[i];
        } else {
            r += s[i];
        }
    }
    return r;
}

void CalendarService::load() noexcept {
    if (loaded_) return;
    loaded_ = true;
    std::ifstream f(calendarPath());
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        // Split on unescaped '|'
        std::vector<std::string> fields;
        std::string cur;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\\' && i+1 < line.size()) {
                cur += line[i]; cur += line[i+1]; ++i;
            } else if (line[i] == '|') {
                fields.push_back(unescape(cur)); cur.clear();
            } else {
                cur += line[i];
            }
        }
        fields.push_back(unescape(cur));
        if (fields.size() < 13) continue;
        Event ev{};
        try {
            ev.id            = std::stoul(fields[0]);
            ev.year          = (uint16_t)std::stoul(fields[1]);
            ev.month         = (uint8_t)std::stoul(fields[2]);
            ev.day           = (uint8_t)std::stoul(fields[3]);
            ev.hour          = (uint8_t)std::stoul(fields[4]);
            ev.minute        = (uint8_t)std::stoul(fields[5]);
            ev.end_hour      = (uint8_t)std::stoul(fields[6]);
            ev.end_minute    = (uint8_t)std::stoul(fields[7]);
            ev.reminder_min  = (uint16_t)std::stoul(fields[8]);
            ev.all_day       = (uint8_t)std::stoul(fields[9]);
            ev.title         = fields[10];
            ev.description   = fields[11];
            ev.location      = fields[12];
        } catch (...) { continue; }
        events_.push_back(std::move(ev));
    }
}

void CalendarService::save() noexcept {
    std::ofstream f(calendarPath(), std::ios::trunc);
    f << "# FreeBSD-X Calendar — do not edit manually\n";
    for (const auto& e : events_) {
        f << e.id << '|' << e.year << '|' << (int)e.month << '|' << (int)e.day
          << '|' << (int)e.hour << '|' << (int)e.minute
          << '|' << (int)e.end_hour << '|' << (int)e.end_minute
          << '|' << (int)e.reminder_min << '|' << (int)e.all_day
          << '|' << escape(e.title)
          << '|' << escape(e.description)
          << '|' << escape(e.location) << '\n';
    }
}

// ── ICS parser ────────────────────────────────────────────────────────────────

// Parse DTSTART / DTEND value:  YYYYMMDD or YYYYMMDDTHHMMSSz
static bool parseDt(const std::string& val,
                    uint16_t& year, uint8_t& month, uint8_t& day,
                    uint8_t& hour, uint8_t& minute) {
    std::string v;
    for (char c : val) if (c != ':' && c != 'Z' && c != 'z') v += c;
    // Remove UTC offset if present (simplification)
    if (v.size() >= 8) {
        try {
            year  = (uint16_t)std::stoul(v.substr(0,4));
            month = (uint8_t) std::stoul(v.substr(4,2));
            day   = (uint8_t) std::stoul(v.substr(6,2));
        } catch (...) { return false; }
        if (v.size() >= 15 && v[8] == 'T') {
            try {
                hour   = (uint8_t)std::stoul(v.substr(9,2));
                minute = (uint8_t)std::stoul(v.substr(11,2));
            } catch (...) {}
        }
        return true;
    }
    return false;
}

// Unfold RFC 5545 line continuations (line ends with CRLF or LF; next line
// begins with space or tab → continuation).
static std::vector<std::string> unfoldIcs(std::istream& in) {
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!lines.empty() && !line.empty() &&
            (line[0] == ' ' || line[0] == '\t')) {
            lines.back() += line.substr(1);
        } else {
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

int CalendarService::importIcs(const std::string& path) noexcept {
    std::ifstream f(path);
    if (!f.is_open()) return 0;
    auto lines = unfoldIcs(f);

    int imported = 0;
    bool in_event = false;
    bool in_alarm = false;
    Event ev{};
    uint16_t e_year{}; uint8_t e_month{}, e_day{}, e_hour{}, e_min{};
    uint16_t eh_year{}; uint8_t eh_month{}, eh_day{}, eh_hour{}, eh_min{};

    auto getVal = [](const std::string& line) -> std::string {
        auto colon = line.find(':');
        [[maybe_unused]] auto semi  = line.find(';');
        // take the part after last ':' (handles DTSTART;TZID=...:value)
        if (colon == std::string::npos) return {};
        return line.substr(colon + 1);
    };
    auto getKey = [](const std::string& line) -> std::string {
        auto colon = line.find(':');
        auto semi  = line.find(';');
        size_t end = std::min({colon, semi, line.size()});
        std::string k = line.substr(0, end);
        std::transform(k.begin(), k.end(), k.begin(), ::toupper);
        return k;
    };

    for (const auto& raw : lines) {
        std::string key = getKey(raw);
        if (key == "BEGIN" && getVal(raw) == "VEVENT") {
            in_event = true; in_alarm = false;
            ev = {}; ev.end_hour = 23; ev.end_minute = 59;
            e_year=0; e_month=0; e_day=0; e_hour=0; e_min=0;
            eh_year=0; eh_month=0; eh_day=0; eh_hour=0; eh_min=0;
            continue;
        }
        if (key == "END" && getVal(raw) == "VEVENT") {
            if (in_event && e_year && e_month && e_day && !ev.title.empty()) {
                ev.year = e_year; ev.month = e_month; ev.day = e_day;
                ev.hour = e_hour; ev.minute = e_min;
                if (eh_year) { ev.end_hour = eh_hour; ev.end_minute = eh_min; }
                ev.id = nextId();
                events_.push_back(ev);
                ++imported;
            }
            in_event = false; in_alarm = false;
            continue;
        }
        if (key == "BEGIN" && getVal(raw) == "VALARM") { in_alarm = true; continue; }
        if (key == "END"   && getVal(raw) == "VALARM") { in_alarm = false; continue; }
        if (!in_event) continue;

        if (key == "SUMMARY") {
            ev.title = getVal(raw).substr(0, 79);
        } else if (key == "DESCRIPTION") {
            std::string d = getVal(raw);
            // Unescape ICS \n
            std::string out;
            for (size_t i = 0; i < d.size(); ++i)
                if (d[i]=='\\' && i+1<d.size() && d[i+1]=='n') { out+='\n'; ++i; }
                else out += d[i];
            ev.description = out.substr(0, 79);
        } else if (key == "LOCATION") {
            ev.location = getVal(raw).substr(0, 31);
        } else if (key.starts_with("DTSTART")) {
            parseDt(getVal(raw), e_year, e_month, e_day, e_hour, e_min);
            if (raw.find("VALUE=DATE") != std::string::npos) ev.all_day = 1;
        } else if (key.starts_with("DTEND")) {
            parseDt(getVal(raw), eh_year, eh_month, eh_day, eh_hour, eh_min);
        } else if (in_alarm && key == "TRIGGER") {
            // e.g. -PT15M or -PT1H
            std::string v = getVal(raw);
            uint16_t mins = 0;
            // Remove leading '-' and 'PT'
            auto pt = v.find("PT");
            if (pt != std::string::npos) {
                std::string after = v.substr(pt + 2);
                auto h = after.find('H');
                auto m = after.find('M');
                try {
                    if (h != std::string::npos)
                        mins += (uint16_t)(std::stoul(after.substr(0, h)) * 60);
                    if (m != std::string::npos) {
                        size_t start = (h != std::string::npos) ? h+1 : 0;
                        mins += (uint16_t)std::stoul(after.substr(start, m - start));
                    }
                } catch (...) {}
            }
            if (mins > 0) ev.reminder_min = mins;
        }
    }
    return imported;
}

// ── IPC message handler ───────────────────────────────────────────────────────

bool CalendarService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                     const shared::XBusMessage& msg) noexcept {
    using namespace shared;
    if (std::strncmp(msg.service_name_, "com.freebsdx.Calendar",
                     sizeof(msg.service_name_)) != 0)
        return false;

    std::lock_guard<std::mutex> lk(mutex_);
    load();

    XBusMessage resp = msg;
    resp.magic_ = XBUS_MAGIC;

    switch (static_cast<CalendarOpcode>(msg.opcode_)) {

    case CalendarOpcode::AddEvent: {
        CalendarEvent req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        Event ev{};
        ev.id          = nextId();
        ev.year        = req.year_;
        ev.month       = req.month_;
        ev.day         = req.day_;
        ev.hour        = req.hour_;
        ev.minute      = req.minute_;
        ev.end_hour    = req.end_hour_;
        ev.end_minute  = req.end_minute_;
        ev.reminder_min = req.reminder_min_;
        ev.all_day     = req.all_day_;
        ev.title       = std::string(req.title_,
            strnlen(req.title_, sizeof(req.title_)));
        ev.description = std::string(req.description_,
            strnlen(req.description_, sizeof(req.description_)));
        ev.location    = std::string(req.location_,
            strnlen(req.location_, sizeof(req.location_)));
        events_.push_back(ev);
        save();
        // Return the new id in response
        CalendarEvent out{}; out.id_ = ev.id;
        resp.opcode_ = static_cast<uint32_t>(CalendarOpcode::AddEvent);
        std::memcpy(resp.payload_, &out, sizeof(out));
        client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case CalendarOpcode::UpdateEvent: {
        CalendarEvent req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        for (auto& e : events_) {
            if (e.id != req.id_) continue;
            e.year        = req.year_;
            e.month       = req.month_;
            e.day         = req.day_;
            e.hour        = req.hour_;
            e.minute      = req.minute_;
            e.end_hour    = req.end_hour_;
            e.end_minute  = req.end_minute_;
            e.reminder_min = req.reminder_min_;
            e.all_day     = req.all_day_;
            e.title       = std::string(req.title_,
                strnlen(req.title_, sizeof(req.title_)));
            e.description = std::string(req.description_,
                strnlen(req.description_, sizeof(req.description_)));
            e.location    = std::string(req.location_,
                strnlen(req.location_, sizeof(req.location_)));
            break;
        }
        save();
        resp.opcode_ = static_cast<uint32_t>(CalendarOpcode::UpdateEvent);
        client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case CalendarOpcode::DeleteEvent: {
        CalendarDeleteRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        events_.erase(std::remove_if(events_.begin(), events_.end(),
            [&](const Event& e){ return e.id == req.event_id_; }),
            events_.end());
        save();
        resp.opcode_ = static_cast<uint32_t>(CalendarOpcode::DeleteEvent);
        client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case CalendarOpcode::ListEvents: {
        CalendarListRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));

        // Build filtered list
        std::vector<const Event*> filtered;
        for (const auto& e : events_) {
            if (req.year_  && e.year  != req.year_)  continue;
            if (req.month_ && e.month != req.month_) continue;
            filtered.push_back(&e);
        }
        // Sort by date/time
        std::sort(filtered.begin(), filtered.end(),
            [](const Event* a, const Event* b){
                if (a->year   != b->year)   return a->year   < b->year;
                if (a->month  != b->month)  return a->month  < b->month;
                if (a->day    != b->day)    return a->day    < b->day;
                if (a->hour   != b->hour)   return a->hour   < b->hour;
                return a->minute < b->minute;
            });

        uint32_t total = (uint32_t)filtered.size();
        uint32_t page  = req.page_;

        CalendarEventPage ep{};
        ep.page_  = page;
        ep.total_ = total;

        if (page < total) {
            const Event* e = filtered[page];
            ep.valid_       = 1;
            ep.event_id_    = e->id;
            ep.year_        = e->year;
            ep.month_       = e->month;
            ep.day_         = e->day;
            ep.hour_        = e->hour;
            ep.minute_      = e->minute;
            ep.end_hour_    = e->end_hour;
            ep.end_minute_  = e->end_minute;
            ep.reminder_min_ = e->reminder_min;
            ep.all_day_     = e->all_day;
            std::strncpy(ep.title_,
                e->title.c_str(), sizeof(ep.title_) - 1);
            std::strncpy(ep.description_,
                e->description.c_str(), sizeof(ep.description_) - 1);
            std::strncpy(ep.location_,
                e->location.c_str(), sizeof(ep.location_) - 1);
        }

        resp.opcode_ = static_cast<uint32_t>(CalendarOpcode::EventPage);
        std::memcpy(resp.payload_, &ep, sizeof(ep));
        client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case CalendarOpcode::ImportIcs: {
        CalendarImportRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.path_[sizeof(req.path_) - 1] = '\0';
        int n = importIcs(std::string(req.path_, strnlen(req.path_, sizeof(req.path_))));
        if (n > 0) save();
        CalendarImportResult res{}; res.imported_ = (uint32_t)n;
        resp.opcode_ = static_cast<uint32_t>(CalendarOpcode::ImportIcs);
        std::memcpy(resp.payload_, &res, sizeof(res));
        client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    default:
        break;
    }
    return true;
}

// ── Reminder poller ───────────────────────────────────────────────────────────
// Called ~1 Hz from compositor. Fires EventReminder broadcast when an event
// is within reminder_min_ minutes of its start time.

void CalendarService::pollReminders() noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!loaded_) return;

    std::time_t now = std::time(nullptr);
    std::tm* lt = std::localtime(&now);
    if (!lt) return;

    for (const auto& e : events_) {
        if (e.reminder_min == 0) continue;

        // Build event start as time_t
        std::tm ev_tm{};
        ev_tm.tm_year  = e.year - 1900;
        ev_tm.tm_mon   = e.month - 1;
        ev_tm.tm_mday  = e.day;
        ev_tm.tm_hour  = e.hour;
        ev_tm.tm_min   = e.minute;
        ev_tm.tm_isdst = -1;
        std::time_t ev_t = std::mktime(&ev_tm);
        if (ev_t == (std::time_t)-1) continue;

        double secs_until = std::difftime(ev_t, now);
        double target = e.reminder_min * 60.0;
        // Fire when within a 2-second window of the reminder time
        if (secs_until >= target - 1.0 && secs_until <= target + 1.0) {
            shared::XBusMessage bcast{};
            bcast.magic_ = shared::XBUS_MAGIC;
            std::strncpy(bcast.service_name_, "com.freebsdx.Calendar",
                         sizeof(bcast.service_name_) - 1);
            bcast.opcode_ = static_cast<uint32_t>(
                shared::CalendarOpcode::EventReminder);
            shared::CalendarEventPage ep{};
            ep.event_id_ = e.id;
            ep.year_     = e.year;
            ep.month_    = e.month;
            ep.day_      = e.day;
            ep.hour_     = e.hour;
            ep.minute_   = e.minute;
            std::strncpy(ep.title_, e.title.c_str(), sizeof(ep.title_) - 1);
            std::memcpy(bcast.payload_, &ep, sizeof(ep));
            BroadcastXBusMessage(bcast);
        }
    }
}

std::vector<CalendarService::ReminderInfo> CalendarService::reminders() noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    load();
    std::vector<ReminderInfo> out;
    for (const auto& e : events_) {
        if (e.reminder_min == 0) continue;
        std::tm tm{};
        tm.tm_year  = e.year - 1900;
        tm.tm_mon   = e.month - 1;
        tm.tm_mday  = e.day;
        tm.tm_hour  = e.all_day ? 0 : e.hour;
        tm.tm_min   = e.all_day ? 0 : e.minute;
        tm.tm_isdst = -1;
        std::time_t start = std::mktime(&tm);
        if (start == static_cast<std::time_t>(-1)) continue;
        out.push_back({e.id, e.title, start, start - static_cast<std::time_t>(e.reminder_min) * 60});
    }
    return out;
}

}  // namespace fbx::xserver
