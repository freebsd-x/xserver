#pragma once

#include <algorithm>
#include <cstdint>

// What the screen should be doing after so long without input: the decisions only, so
// they can be tested without a screen. IdleService acts on them.
//
// One sequence, as GNOME and KDE have it: the screen dims, the lock screen comes up,
// the screen goes dark. Each delay can be off (0 = never).
namespace fbx::xserver::idle {

enum class Phase : uint8_t { Active, Dim, Off };

struct Settings {
    uint32_t lock_secs = 0;         // without input before locking (0 = never)
    uint32_t off_secs = 0;          // before the screen goes dark (0 = never)
    uint32_t locked_off_secs = 30;  // the same, once it's locked: nobody's working at it
};

struct Now {
    uint32_t idle_secs = 0;         // since the last input
    uint32_t locked_secs = 0;       // since it was locked (if it is)
    bool locked = false;
    bool held = false;              // something (a video) holds idle off
    bool at_greeter = false;        // nobody's logged in: nothing to lock
};

// A warning before either, while it can still be called off with a key.
inline constexpr uint32_t kDimSecs = 15;

struct Decision {
    Phase phase = Phase::Active;
    bool lock = false;
};

inline Decision decide(const Settings& s, const Now& n) noexcept {
    Decision d;
    if (n.locked) {
        // Nothing holds a locked screen on. (Locked by hand: from then, or the last input.)
        if (s.off_secs == 0) return d;
        if (std::min(n.idle_secs, n.locked_secs) >= s.locked_off_secs) d.phase = Phase::Off;
        // Dark already when the lock came: it stays so.
        else if (n.idle_secs >= s.off_secs && s.lock_secs > s.off_secs) d.phase = Phase::Off;
        return d;
    }
    if (n.held) return d;
    const uint32_t lock = n.at_greeter ? 0 : s.lock_secs;
    if (lock != 0 && n.idle_secs >= lock) d.lock = true;
    // Both at once: the lock screen is seen first, and goes dark in its own time.
    if (s.off_secs != 0 && n.idle_secs >= s.off_secs && !(lock != 0 && lock <= s.off_secs && n.idle_secs >= lock)) {
        d.phase = Phase::Off;
        return d;
    }
    const auto soon = [&](uint32_t at) { return at > 2 * kDimSecs && n.idle_secs + kDimSecs >= at; };
    if (!d.lock && (soon(lock) || soon(s.off_secs))) d.phase = Phase::Dim;
    return d;
}

}  // namespace fbx::xserver::idle
