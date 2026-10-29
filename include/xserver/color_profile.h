#pragma once

#include <array>
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

// The display's gamut, as a 3×3 row-major matrix taking sRGB content into
// it (linear-light D65 primaries conversion; rows sum to 1 so white stays
// white). It goes last in the CRTC colour matrix, after colour filters and
// Night Light. The CTM sees gamma-encoded values (no de-gamma LUT is set),
// so this is the usual close approximation, not an exact CMS transform.
inline std::array<double, 9> matrixFor(uint32_t p) noexcept {
    switch (clampProfile(p)) {
        case 1:  return {0.8225, 0.1774, 0.0000,     // sRGB → Display P3
                         0.0332, 0.9669, 0.0000,
                         0.0171, 0.0724, 0.9108};
        case 2:  return {0.7152, 0.2848, 0.0000,     // sRGB → Adobe RGB (1998)
                         0.0000, 1.0000, 0.0000,
                         0.0000, 0.0412, 0.9588};
        default: return {1, 0, 0, 0, 1, 0, 0, 0, 1};
    }
}

}  // namespace fbx::xserver::colorprofile
