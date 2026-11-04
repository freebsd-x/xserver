#pragma once

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
