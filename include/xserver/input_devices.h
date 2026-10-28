#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

// Pure helpers for com.freebsdx.InputDevice: stable device ids, the
// sensitivity ↔ libinput speed mapping, and the persisted per-device speed
// overrides. Dependency-free / unit-testable; no libinput calls.
namespace fbx::xserver::inputdev {

// Device id = FNV-1a of the device name, so it survives replugs and restarts
// (libinput has no stable per-device id). Never 0.
inline uint32_t deviceId(std::string_view name) {
    uint32_t h = 2166136261u;
    for (char c : name) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
    return h ? h : 1u;
}

// InputDevicePage::type_.
enum Type : uint32_t { kKeyboard = 0, kPointer = 1, kTouch = 2, kTablet = 3 };

// Most specific capability wins (a tablet also reports pointer, etc.).
inline Type pickType(bool keyboard, bool pointer, bool touch, bool tablet) {
    if (tablet)  return kTablet;
    if (touch)   return kTouch;
    if (pointer) return kPointer;
    (void)keyboard;
    return kKeyboard;
}

// Sensitivity 0-100 ↔ libinput accel speed [-1, 1]; 50 = libinput default.
inline double sensitivityToSpeed(uint32_t s) {
    if (s > 100) s = 100;
    return static_cast<double>(s) / 50.0 - 1.0;
}
inline uint32_t speedToSensitivity(double speed) {
    if (speed < -1.0) speed = -1.0;
    if (speed > 1.0) speed = 1.0;
    return static_cast<uint32_t>((speed + 1.0) * 50.0 + 0.5);
}

// One live device, as InputManager reports it.
struct DeviceInfo {
    uint32_t    id = 0;
    std::string name;
    uint32_t    vendor = 0, product = 0;
    uint32_t    type = kKeyboard;
    uint32_t    sensitivity = 50;   // current accel speed, 0-100
    bool        adaptive = true;
};

struct Override {
    uint32_t sensitivity = 50;
    bool     adaptive    = true;
};

// Persisted as "id:sens:prof;…" (one ConfigService key).
inline std::map<uint32_t, Override> parseOverrides(std::string_view s) {
    std::map<uint32_t, Override> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t semi = s.find(';', start);
        if (semi == std::string_view::npos) semi = s.size();
        std::string_view item = s.substr(start, semi - start);
        start = semi + 1;
        size_t c1 = item.find(':');
        size_t c2 = c1 == std::string_view::npos ? c1 : item.find(':', c1 + 1);
        if (c2 == std::string_view::npos) continue;
        auto num = [](std::string_view v, uint64_t& o) {
            if (v.empty() || v.size() > 10) return false;
            o = 0;
            for (char c : v) { if (c < '0' || c > '9') return false; o = o * 10 + static_cast<uint64_t>(c - '0'); }
            return true;
        };
        uint64_t id, sens, prof;
        if (!num(item.substr(0, c1), id) || !num(item.substr(c1 + 1, c2 - c1 - 1), sens) ||
            !num(item.substr(c2 + 1), prof) || id == 0 || id > 0xffffffffu || sens > 100 || prof > 1)
            continue;
        out[static_cast<uint32_t>(id)] = {static_cast<uint32_t>(sens), prof == 1};
    }
    return out;
}

inline std::string formatOverrides(const std::map<uint32_t, Override>& m) {
    std::string s;
    for (const auto& [id, o] : m) {
        if (!s.empty()) s += ';';
        s += std::to_string(id) + ":" + std::to_string(o.sensitivity) + ":" + (o.adaptive ? "1" : "0");
    }
    return s;
}

}  // namespace fbx::xserver::inputdev
