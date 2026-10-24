#pragma once

#include <cstdint>
#include <ctime>

// Pure Do-Not-Disturb expiry math: given a duration selector and the current
// time, when does DND auto-expire? 0 = indefinite.
namespace fbx::xserver::dnd {

// duration: 0=Until off, 1=1 hour, 2=8 hours, 3=Until tomorrow (next local 00:00).
inline uint32_t endsAt(uint32_t duration, time_t now) noexcept {
    switch (duration) {
    case 1: return static_cast<uint32_t>(now + 3600);
    case 2: return static_cast<uint32_t>(now + 8 * 3600);
    case 3: {
        struct tm lt{};
        localtime_r(&now, &lt);
        lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0;   // today 00:00
        lt.tm_mday += 1;                                 // → tomorrow 00:00
        return static_cast<uint32_t>(mktime(&lt));
    }
    default: return 0;   // Until off → indefinite
    }
}

}  // namespace fbx::xserver::dnd
