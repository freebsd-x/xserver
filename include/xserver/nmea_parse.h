#pragma once
// nmea_parse.h — pure NMEA 0183 sentence parsing for the GPS service.
//
// A serial GPS receiver (e.g. a USB dongle on /dev/cuau0) emits ASCII sentences
// like:
//   $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47
//   $GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A
// We only need a position fix, so we parse GGA (has fix-quality + HDOP) and RMC
// (has an active/void status). Coordinates are ddmm.mmmm / dddmm.mmmm and are
// converted to signed decimal degrees.
//
// Header-only and free of I/O so it can be unit-tested without a device.

#include <string>
#include <string_view>
#include <vector>
#include <cstdlib>
#include <cmath>

namespace fbx::xserver::nmea {

struct Fix {
    bool   valid = false;   // true only for a positional sentence with a real fix
    double lat = 0.0;       // decimal degrees, +N / -S
    double lon = 0.0;       // decimal degrees, +E / -W
    float  accuracy_m = 0.0f; // estimate from HDOP (0 = unknown)
};

namespace detail {

inline int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Validate the "*HH" checksum if present. Returns true when absent (some feeds
// omit it) or when it matches the XOR of the bytes between '$' and '*'.
inline bool checksumOk(std::string_view body, std::string_view sum) {
    if (sum.size() < 2) return true; // no checksum supplied
    int hi = hexVal(sum[0]), lo = hexVal(sum[1]);
    if (hi < 0 || lo < 0) return false;
    unsigned expected = static_cast<unsigned>(hi * 16 + lo);
    unsigned x = 0;
    for (char c : body) x ^= static_cast<unsigned char>(c);
    return (x & 0xFF) == expected;
}

// ddmm.mmmm (lat) or dddmm.mmmm (lon) → signed decimal degrees.
// Returns false if the field is empty/unparseable.
inline bool parseCoord(std::string_view field, char hemi, double& out) {
    if (field.empty()) return false;
    // atof needs NUL-termination; the field is short so copy it.
    std::string s(field);
    char* end = nullptr;
    double raw = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;           // no digits
    int deg = static_cast<int>(raw / 100.0);
    double minutes = raw - deg * 100.0;
    if (minutes < 0.0 || minutes >= 60.0) return false; // malformed
    double d = deg + minutes / 60.0;
    if (hemi == 'S' || hemi == 'W' || hemi == 's' || hemi == 'w') d = -d;
    out = d;
    return true;
}

inline std::vector<std::string_view> split(std::string_view s, char delim) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == delim) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                          s.back() == ' '  || s.back() == '\t'))
        s.remove_suffix(1);
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    return s;
}

} // namespace detail

// Parse one NMEA sentence. Returns a Fix with valid=false for anything that is
// not a GGA/RMC positional sentence with a genuine fix (also on bad checksum or
// malformed coordinates).
inline Fix parseSentence(std::string_view line) {
    using namespace detail;
    line = trim(line);
    if (line.empty() || line.front() != '$') return {};

    // Separate the "*HH" checksum from the sentence body (everything after '$').
    std::string_view body = line.substr(1);
    std::string_view sum;
    if (auto star = body.find('*'); star != std::string_view::npos) {
        sum  = body.substr(star + 1);
        body = body.substr(0, star);
    }
    if (!checksumOk(body, sum)) return {};

    auto f = split(body, ',');
    if (f.empty() || f[0].size() < 5) return {};
    // Match on the 3-char sentence type, ignoring the 2-char talker id
    // (GP/GN/GL/GA/BD…): the type is the last three chars of field 0.
    std::string_view type = f[0].substr(f[0].size() - 3);

    Fix fix;
    if (type == "GGA") {
        // 0:type 1:time 2:lat 3:N/S 4:lon 5:E/W 6:quality 7:sats 8:HDOP
        if (f.size() < 7) return {};
        if (f[6].empty() || f[6] == "0") return {}; // 0 = no fix
        char ns = f[3].empty() ? '?' : f[3][0];
        char ew = f[5].empty() ? '?' : f[5][0];
        if (!parseCoord(f[2], ns, fix.lat)) return {};
        if (!parseCoord(f[4], ew, fix.lon)) return {};
        if (f.size() > 8 && !f[8].empty()) {
            double hdop = std::strtod(std::string(f[8]).c_str(), nullptr);
            if (hdop > 0.0) fix.accuracy_m = static_cast<float>(hdop * 5.0); // ~5m UERE
        }
        fix.valid = true;
        return fix;
    }
    if (type == "RMC") {
        // 0:type 1:time 2:status(A/V) 3:lat 4:N/S 5:lon 6:E/W
        if (f.size() < 7) return {};
        if (f[2].empty() || (f[2][0] != 'A' && f[2][0] != 'a')) return {}; // V = void
        char ns = f[4].empty() ? '?' : f[4][0];
        char ew = f[6].empty() ? '?' : f[6][0];
        if (!parseCoord(f[3], ns, fix.lat)) return {};
        if (!parseCoord(f[5], ew, fix.lon)) return {};
        fix.valid = true;
        return fix;
    }
    return {};
}

// Scan a buffer of one or more newline-separated sentences and return the most
// recent valid fix (later sentences win). valid=false if none had a fix.
inline Fix parseStream(std::string_view buf) {
    Fix best;
    size_t start = 0;
    for (size_t i = 0; i <= buf.size(); ++i) {
        if (i == buf.size() || buf[i] == '\n') {
            Fix f = parseSentence(buf.substr(start, i - start));
            if (f.valid) best = f;
            start = i + 1;
        }
    }
    return best;
}

} // namespace fbx::xserver::nmea
