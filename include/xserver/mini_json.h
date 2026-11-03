#pragma once

#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

// The minimal JSON scanning the compositor does on files it didn't write —
// app manifests and the downloaded package catalog (AppManager, MimeService).
// Flat "key":"value" lookups only, no nesting. Bounds-checked std::string
// scanning throughout, linear time in the input, never throws on malformed
// input (tests/unit/mini_json_fuzz_test.cc runs it on mutated/hostile input
// under ASan+UBSan).
namespace fbx::xserver::mini_json {

// The first "key":"string" value, or "".
inline std::string value(const std::string& json, const std::string& key) {
    const std::string k = "\"" + key + "\":";
    size_t pos = json.find(k);
    if (pos == std::string::npos) return "";
    size_t s = json.find('"', pos + k.size());
    if (s == std::string::npos) return "";
    size_t e = json.find('"', s + 1);
    if (e == std::string::npos) return "";
    return json.substr(s + 1, e - s - 1);
}

// The strings of the first "key": [ "a", "b" ] array.
inline std::vector<std::string> array(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    const std::string k = "\"" + key + "\"";
    size_t pos = json.find(k);
    if (pos == std::string::npos) return result;
    size_t bracket = json.find('[', pos + k.size());
    if (bracket == std::string::npos) return result;
    size_t end = json.find(']', bracket);
    if (end == std::string::npos) return result;
    size_t i = bracket + 1;
    while (i < end) {
        size_t q1 = json.find('"', i);
        if (q1 == std::string::npos || q1 >= end) break;
        size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 >= end) break;
        result.push_back(json.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return result;
}

// The flat objects of the first "key": [ {...}, {...} ] array, as key→value
// maps (string values unquoted, others as written, trimmed).
inline std::vector<std::unordered_map<std::string, std::string>>
objectArray(const std::string& json, const std::string& array_key) {
    std::vector<std::unordered_map<std::string, std::string>> result;
    const std::string k = "\"" + array_key + "\"";
    size_t pos = json.find(k);
    if (pos == std::string::npos) return result;
    size_t bracket = json.find('[', pos + k.size());
    if (bracket == std::string::npos) return result;
    // Found once: it used to be searched again for every object — quadratic in
    // the size of a (downloaded, untrusted) catalog.
    const size_t arr_end = json.find(']', bracket + 1);

    size_t i = bracket + 1;
    while (i < json.size()) {
        size_t obj_start = json.find('{', i);
        if (obj_start == std::string::npos || obj_start > arr_end) break;
        size_t obj_end = json.find('}', obj_start);
        if (obj_end == std::string::npos) break;

        std::unordered_map<std::string, std::string> entry;
        size_t j = obj_start + 1;
        while (j < obj_end) {
            size_t kq1 = json.find('"', j);
            if (kq1 == std::string::npos || kq1 >= obj_end) break;
            size_t kq2 = json.find('"', kq1 + 1);
            if (kq2 == std::string::npos || kq2 >= obj_end) break;
            std::string key = json.substr(kq1 + 1, kq2 - kq1 - 1);
            size_t colon = json.find(':', kq2 + 1);
            if (colon == std::string::npos || colon >= obj_end) break;
            size_t vstart = colon + 1;
            while (vstart < obj_end && json[vstart] == ' ') ++vstart;
            std::string val;
            if (vstart < obj_end && json[vstart] == '"') {
                size_t vq2 = json.find('"', vstart + 1);
                if (vq2 == std::string::npos || vq2 >= obj_end) break;
                val = json.substr(vstart + 1, vq2 - vstart - 1);
                j = vq2 + 1;
            } else {
                size_t vend = json.find_first_of(",}", vstart);
                if (vend == std::string::npos || vend > obj_end) vend = obj_end;
                val = json.substr(vstart, vend - vstart);
                while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
                j = vend + 1;
            }
            entry[std::move(key)] = std::move(val);
        }
        if (!entry.empty()) result.push_back(std::move(entry));
        i = obj_end + 1;
    }
    return result;
}

}  // namespace fbx::xserver::mini_json
