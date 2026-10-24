#pragma once

#include "shared/protocol.h"
#include <cstdint>
#include <string_view>
#include <vector>

// Pure helpers for the login-session service (com.freebsdx.Sessions): classify a
// utmpx line into a session type, derive a stable session id, and page a session
// list into the wire format. Dependency-free so they can be unit-tested directly.
namespace fbx::xserver::session {

// The synthetic utmpx line xsessiond writes for the graphical seat
// (must match kUtLine in xsessiond.cc).
inline constexpr std::string_view kGuiLine = "xsess0";

// Classify a session from its utmpx line + host. A non-empty host means a remote
// login ("ssh"); the GUI seat line / a virtual console is "graphical"; anything
// else is a local "tty".
inline const char* classifyType(std::string_view line, std::string_view host) noexcept {
    if (!host.empty()) return "ssh";
    if (line == kGuiLine || line.rfind("ttyv", 0) == 0 ||
        line.rfind("fbxseat", 0) == 0)
        return "graphical";
    return "tty";
}

// True if this line is the local graphical seat (its KillSession means "log out"
// of the desktop, not SIGHUP a leader).
inline bool isGuiSeat(std::string_view line) noexcept {
    return line == kGuiLine || line.rfind("fbxseat", 0) == 0;
}

// Stable 32-bit id for a session, derived from its line + user (FNV-1a). Stable
// across re-enumeration while the session persists, so a client can ListSessions
// then KillSession by id. Never returns 0 (0 is reserved for "no session").
inline uint32_t sessionId(std::string_view line, std::string_view user) noexcept {
    uint32_t h = 2166136261u;
    for (char c : line) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
    h ^= 0xff;  // separator between the two fields
    h *= 16777619u;
    for (char c : user) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
    return h ? h : 1u;
}

// Slice a full session list into one wire page (3 entries per page).
inline shared::SessionPage buildPage(const std::vector<shared::SessionEntry>& all,
                                     uint32_t page) noexcept {
    shared::SessionPage pg{};
    pg.page_  = page;
    pg.total_ = static_cast<uint32_t>(all.size());
    uint32_t start = page * 3;
    uint32_t n = 0;
    for (; n < 3 && start + n < all.size(); ++n) pg.entries_[n] = all[start + n];
    pg.count_    = n;
    pg.has_more_ = (start + n < all.size()) ? 1u : 0u;
    return pg;
}

}  // namespace fbx::xserver::session
