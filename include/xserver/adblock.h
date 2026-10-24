#pragma once

#include <ctime>
#include <string>

// Pure helper for com.freebsdx.AdBlock. Dependency-free / unit-testable; the
// service holds the config-backed state. Real DNS sinkholing (writing a hosts
// file / unbound config + fetching a blocklist) needs root + network and is a
// follow-up — this backs the toggle, domain count, and refresh date.
namespace fbx::xserver::adblock {

// "Jan 14 2026"-style date for the AdBlockStatus.list_updated_ field. Uses UTC
// so the format is deterministic (timezone-independent); the label is coarse.
inline std::string formatListDate(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[24];
    std::strftime(buf, sizeof(buf), "%b %d %Y", &tm);
    return buf;
}

}  // namespace fbx::xserver::adblock
