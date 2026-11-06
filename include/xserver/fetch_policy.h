#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Which hosts each app may fetch through com.freebsdx.Fetch. The compositor
// checks a request against the calling app's (verified) identity; xfetch
// re-checks that the host is in the table at all, so a confused or
// compromised relay can't turn it into an open proxy. Exact host match, no
// userinfo/port tricks, and https unless the rule says otherwise.
namespace fbx::xserver::fetch_policy {

struct Rule {
    const char* app;      // verified app id (or kCompositor)
    const char* host;
    bool        https_only;
    bool        cacheable;  // xfetch may answer from (and fill) its disk cache
    size_t      max_bytes;  // largest body xfetch will accept
};

// The compositor's own services (weather, the package catalog and downloads,
// the ad-block list) fetch through xfetch too, under this identity. No app can
// hold it: the broker refuses an app whose manifest claims it.
inline constexpr const char* kCompositor = "freebsdx.compositor";

inline constexpr size_t kMiB = 1u << 20;
inline constexpr Rule kRules[] = {
    {"com.freebsdx.maps", "tile.openstreetmap.org",      true,  true,  8 * kMiB},
    {"com.freebsdx.maps", "nominatim.openstreetmap.org", true,  false, 1 * kMiB},
    // ip-api's free tier is HTTP-only; Maps uses it only on an explicit,
    // opted-in "My Location" tap (see ipGeoEnabled() in Maps).
    {"com.freebsdx.maps", "ip-api.com",                  false, false, 1 * kMiB},
    {kCompositor,         "wttr.in",                     true,  false, 1 * kMiB},
    {kCompositor,         "pkg.freebsd-x.org",           true,  false, 256 * kMiB},   // catalog + packages
    {kCompositor,         "raw.githubusercontent.com",   true,  false, 16 * kMiB},    // the default ad-block list
};

struct Url {
    bool        https = false;
    std::string host;
    std::string path;   // from the first '/' on (always non-empty)
};

// ── Browsing ──────────────────────────────────────────────────────────────────
// One client may ask for any public web address: our web engine's viewer, recognised
// by its executable. It's a single process in a sandbox, it runs no scripts yet, and
// xfetch refuses to connect to anything on this machine or the local network.
inline constexpr const char* kWebApp = "com.freebsdx.web";
inline constexpr size_t kWebMaxBytes = 32 * kMiB;
inline constexpr size_t kWebMaxUrl = 8192;   // real stylesheet and search addresses get long

// An http(s) URL a browser may ask for: no userinfo (a classic way to disguise where a
// link goes), a real port if any. Addresses are checked when connecting, not here: a
// name can resolve to anything.
inline bool webUrlAllowed(std::string_view url) {
    if (url.empty() || url.size() >= kWebMaxUrl) return false;
    for (char c : url)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f || c == '\\') return false;
    std::string lower(url.substr(0, 8));
    for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    std::string_view rest;
    if (lower.starts_with("https://")) rest = url.substr(8);
    else if (lower.starts_with("http://")) rest = url.substr(7);
    else return false;
    const std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
    if (authority.empty() || authority.find_first_of("@[]") != std::string_view::npos) return false;
    const size_t colon = authority.find(':');
    if (colon == std::string_view::npos) return true;
    if (colon == 0) return false;
    const std::string_view digits = authority.substr(colon + 1);
    if (digits.empty() || digits.size() > 5) return false;
    uint32_t port = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return false;
        port = port * 10 + static_cast<uint32_t>(c - '0');
    }
    // The web's ports, not mail's or ssh's.
    return port == 80 || port == 443 || (port >= 1024 && port <= 65535);
}

// This machine, the local network, and addresses that aren't really anywhere: what a
// page must never be able to reach, even by redirecting there.
inline bool isPrivateAddress(const struct sockaddr* sa) {
    if (sa->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const struct sockaddr_in*>(sa);
        const uint32_t a = ntohl(in->sin_addr.s_addr);
        const auto within = [a](uint32_t net, int bits) { return (a >> (32 - bits)) == (net >> (32 - bits)); };
        return within(0x00000000, 8) || within(0x0A000000, 8) || within(0x7F000000, 8) || within(0x64400000, 10) ||
               within(0xA9FE0000, 16) || within(0xAC100000, 12) || within(0xC0A80000, 16) || within(0xC0000000, 24) ||
               a >= 0xE0000000;   // multicast and reserved
    }
    if (sa->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const struct sockaddr_in6*>(sa);
        const uint8_t* b = in6->sin6_addr.s6_addr;
        static constexpr uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (std::equal(b, b + 12, kMapped)) {   // an IPv4 address in disguise
            struct sockaddr_in v4{};
            v4.sin_family = AF_INET;
            std::copy(b + 12, b + 16, reinterpret_cast<uint8_t*>(&v4.sin_addr.s_addr));
            return isPrivateAddress(reinterpret_cast<const struct sockaddr*>(&v4));
        }
        const bool all_zero_but_last = std::all_of(b, b + 15, [](uint8_t x) { return x == 0; });
        return (all_zero_but_last && b[15] <= 1) ||   // :: and ::1
               (b[0] & 0xFE) == 0xFC ||               // unique local
               (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) ||   // link local
               b[0] == 0xFF;                          // multicast
    }
    return true;   // not an internet address at all
}

// Direct connections, for what HTTP relaying can't carry. Mail speaks IMAP and
// SMTP to whichever server the user set up, so it's the port that's limited.
inline bool mayConnect(std::string_view app, uint16_t port) {
    if (app != "com.freebsdx.mail") return false;
    for (uint16_t p : {uint16_t{25}, uint16_t{143}, uint16_t{465}, uint16_t{587}, uint16_t{993}})
        if (port == p) return true;
    return false;
}

// Parse an absolute http(s) URL. Rejects anything with userinfo, an explicit
// port, whitespace/control characters, or an empty host.
inline bool parse(std::string_view url, Url& out) {
    if (url.size() >= 256) return false;
    for (char c : url)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f || c == '\\') return false;
    auto hasScheme = [&](std::string_view scheme) {   // schemes are case-insensitive
        if (url.size() < scheme.size()) return false;
        for (size_t i = 0; i < scheme.size(); ++i) {
            char c = url[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != scheme[i]) return false;
        }
        return true;
    };
    std::string_view rest;
    if (hasScheme("https://")) { out.https = true;  rest = url.substr(8); }
    else if (hasScheme("http://")) { out.https = false; rest = url.substr(7); }
    else return false;
    size_t end = rest.find_first_of("/?#");
    std::string_view host = rest.substr(0, end);
    if (host.empty() || host.find_first_of("@:[]") != std::string_view::npos) return false;
    out.host.assign(host);
    for (char& c : out.host)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    out.path = end == std::string_view::npos ? std::string("/") : std::string(rest.substr(end));
    if (out.path[0] != '/') out.path.insert(0, "/");
    return true;
}

inline const Rule* ruleFor(std::string_view app, const Url& u) {
    for (const Rule& r : kRules)
        if ((app.empty() || app == r.app) && u.host == r.host && (u.https || !r.https_only))
            return &r;
    return nullptr;
}

// May `app` fetch `url`?
inline bool allowed(std::string_view app, std::string_view url) {
    Url u;
    return !app.empty() && parse(url, u) && ruleFor(app, u);
}

// Is `url` allowed for some app (xfetch's own check)? Its rule's cacheability
// and size limit (the most generous of the rules for that host).
inline bool knownHost(std::string_view url, bool* cacheable = nullptr, size_t* max_bytes = nullptr) {
    Url u;
    if (!parse(url, u)) return false;
    bool found = false, cache = false;
    size_t limit = 0;
    for (const Rule& r : kRules)
        if (u.host == r.host && (u.https || !r.https_only)) {
            found = true;
            cache = cache || r.cacheable;
            limit = std::max(limit, r.max_bytes);
        }
    if (cacheable) *cacheable = cache;
    if (max_bytes) *max_bytes = limit;
    return found;
}

}  // namespace fbx::xserver::fetch_policy
