#pragma once

#include <string_view>

// Pure validation for com.freebsdx.Keymap XKB codes. Dependency-free /
// unit-testable; the service persists + broadcasts the chosen layout.
namespace fbx::xserver::keymap {

// XKB layout code: 1–15 lowercase ASCII letters (e.g. "us", "fr", "de", "latam").
inline bool isValidLayout(std::string_view s) noexcept {
    if (s.empty() || s.size() > 15) return false;
    for (char c : s) if (c < 'a' || c > 'z') return false;
    return true;
}

// XKB variant: empty, or ≤15 chars of lowercase letters (e.g. "dvorak",
// "nodeadkeys"). Empty is the common "no variant" case.
inline bool isValidVariant(std::string_view s) noexcept {
    if (s.size() > 15) return false;
    for (char c : s) if (c < 'a' || c > 'z') return false;
    return true;
}

}  // namespace fbx::xserver::keymap
