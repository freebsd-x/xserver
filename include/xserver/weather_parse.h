#pragma once
// Reads wttr.in's ?format=j1 response. Values sit in arrays of {"value": ...}, and
// "value" also labels the area, country and region, so each is found from the key
// it belongs to rather than by counting "value"s through the file.

#include <string>
#include <vector>

namespace fbx::xserver::wttr {

struct Day {
    std::string date, max_c, min_c, max_f, min_f, desc;
};

struct Report {
    std::string area, desc;
    std::vector<Day> days;
};

// The string after "key": at or past `from`, stopping at `to`; "" if there's none.
inline std::string str(const std::string& json, const std::string& key, size_t from = 0,
                       size_t to = std::string::npos) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle, from);
    if (pos == std::string::npos || pos >= to) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    size_t start = json.find('"', pos);
    if (start == std::string::npos || start >= to) return {};
    size_t end = json.find('"', ++start);
    if (end == std::string::npos) return {};
    return json.substr(start, end - start);
}

// "key":[{"value":"..."}] → ...
inline std::string nested(const std::string& json, const std::string& key, size_t from = 0,
                          size_t to = std::string::npos) {
    size_t pos = json.find("\"" + key + "\"", from);
    if (pos == std::string::npos || pos >= to) return {};
    std::string v = str(json, "value", pos, to);
    while (!v.empty() && v.back() == ' ') v.pop_back();     // "Partly Cloudy "
    return v;
}

inline Report parse(const std::string& json) {
    Report r;
    const size_t area_at = json.find("\"nearest_area\"");
    if (area_at != std::string::npos) r.area = nested(json, "areaName", area_at);

    const size_t now_at = json.find("\"current_condition\"");
    if (now_at != std::string::npos) r.desc = nested(json, "weatherDesc", now_at, area_at);

    size_t at = json.find("\"weather\"");
    if (at == std::string::npos) return r;
    at = json.find("\"date\"", at);
    while (at != std::string::npos) {
        const size_t next = json.find("\"date\"", at + 1);
        Day d;
        d.date  = str(json, "date", at, next);
        d.max_c = str(json, "maxtempC", at, next);
        d.min_c = str(json, "mintempC", at, next);
        d.max_f = str(json, "maxtempF", at, next);
        d.min_f = str(json, "mintempF", at, next);
        // Eight three-hourly entries: the day is described by midday's, the fifth.
        size_t hour = at, noon = std::string::npos;
        for (int i = 0; i < 5; ++i) {
            hour = json.find("\"weatherDesc\"", hour + 1);
            if (hour == std::string::npos || hour >= next) break;
            noon = hour;
        }
        if (noon != std::string::npos) d.desc = nested(json, "weatherDesc", noon, next);
        r.days.push_back(std::move(d));
        at = next;
    }
    return r;
}

}  // namespace fbx::xserver::wttr
