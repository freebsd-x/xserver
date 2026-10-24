#pragma once

#include <cstdint>

// Pure helpers for com.freebsdx.ColorProfile. Dependency-free / unit-testable.
namespace fbx::xserver::colorprofile {

// 0=sRGB, 1=Display P3, 2=Adobe RGB. Clamp anything else to sRGB (the safe
// default a display always supports).
inline uint32_t clampProfile(uint32_t p) noexcept { return p <= 2 ? p : 0u; }

inline const char* profileName(uint32_t p) noexcept {
    switch (clampProfile(p)) {
        case 1:  return "Display P3";
        case 2:  return "Adobe RGB";
        default: return "sRGB";
    }
}

}  // namespace fbx::xserver::colorprofile
