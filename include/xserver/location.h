#pragma once

#include <cstdint>

// Pure helpers for the Location services. Dependency-free / unit-testable.
namespace fbx::xserver::location {

// Per-app location precision: 0=off, 1=approximate, 2=precise. Clamp anything
// else to off (safest default).
inline uint32_t clampPrecision(uint32_t p) noexcept {
    return p <= 2 ? p : 0;
}

}  // namespace fbx::xserver::location
