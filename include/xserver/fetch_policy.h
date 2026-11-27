#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cctype>
#include <cstring>
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
// by its executable. It's a single process in a sandbox (its pages' scripts run inside
// it, and it sends cookies only for a page's own site), and xfetch refuses to connect
// to anything on this machine or the local network.
inline constexpr const char* kWebApp = "com.freebsdx.web";
inline constexpr size_t kWebMaxBytes = 32 * kMiB;
inline constexpr size_t kWebMaxUrl = 8192;   // real stylesheet and search addresses get long
inline constexpr size_t kWebMaxPost = 4 * kMiB;   // a form's whole request, address and body

// A POST's content type: a form's, or a script's (fetch, XMLHttpRequest: JSON and the
// like): a MIME type ("type/subtype", then parameters), printable, on one line.
inline bool formContentTypeAllowed(const std::string& type) {
    if (type.empty() || type.size() > 200) return false;
    for (char c : type)
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7f) return false;
    const size_t slash = type.find('/');
    if (slash == std::string::npos || slash == 0) return false;
    const auto token = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || std::strchr("!#$&-^_.+", c) != nullptr; };
    for (size_t i = 0; i < slash; ++i)
        if (!token(type[i])) return false;
    size_t i = slash + 1;
    const size_t start = i;
    while (i < type.size() && token(type[i])) ++i;
    return i > start && (i == type.size() || type[i] == ';' || type[i] == ' ');
}

// A script's request (kFetchScript): the methods, and the headers a page may set (not
// the ones the browser owns: Host, Cookie, the connection's own; Origin and Referer the
// engine sets itself and are let through).
inline constexpr size_t kWebMaxScriptRequest = 4 * kMiB;
inline bool scriptMethodAllowed(const std::string& m) {
    for (const char* ok : {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"})
        if (m == ok) return true;
    return false;
}
inline bool scriptHeaderAllowed(const std::string& name, const std::string& value) {
    if (name.empty() || name.size() > 128 || value.size() > 8192) return false;
    for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || std::strchr("!#$%&'*+-.^_`|~", c))) return false;
    for (char c : value)
        if (c == '\r' || c == '\n' || c == '\0') return false;
    std::string lower;
    for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* no : {"host", "cookie", "cookie2", "content-length", "connection", "keep-alive", "transfer-encoding", "te",
                           "trailer", "upgrade", "via", "expect", "date", "accept-charset", "accept-encoding", "set-cookie"})
        if (lower == no) return false;
    return !lower.starts_with("proxy-") && !lower.starts_with("sec-");
}

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

// ── Sites ─────────────────────────────────────────────────────────────────────
// Cookies are a site's: a host's registrable domain ("www.example.co.kr" and
// "img.example.co.kr" are one). The same rule as our engine's registrableDomain()
// (sdk/xweb/src/url.cc), which decides which requests ask for cookies: if the two
// disagreed, a page would lose its own cookies. Keep them the same.
inline std::string registrableDomain(std::string_view host_in) {
    std::string host;
    for (char c : host_in) host += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!host.empty() && host.back() == '.') host.pop_back();
    if (host.empty() || host.front() == '[' || host.find('.') == std::string::npos ||
        std::all_of(host.begin(), host.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; }))
        return host;
    // The labels from the right: the last three are all that's ever kept.
    std::string_view labels[3];
    size_t n = 0, end = host.size();
    while (true) {
        const size_t dot = host.rfind('.', end - 1);
        const size_t start = dot == std::string::npos ? 0 : dot + 1;
        if (n < 3) labels[n] = std::string_view(host).substr(start, end - start);
        ++n;
        if (dot == std::string::npos) break;
        if (dot == 0) { ++n; break; }   // a leading dot: an empty label, as xweb counts it
        end = dot;
    }
    size_t keep = 2;
    static constexpr const char* kSecondLevel[] = {"co", "com", "net", "org", "gov", "ac", "or", "go", "ne", "edu", "gob", "mil", "ltd", "plc", "sch", "nhs"};
    if (n >= 3 && labels[0].size() == 2 &&
        std::any_of(std::begin(kSecondLevel), std::end(kSecondLevel), [&](const char* l) { return labels[1] == l; }))
        keep = 3;
    static constexpr const char* kShared[] = {"github.io", "gitlab.io", "pages.dev", "netlify.app", "vercel.app", "herokuapp.com", "blogspot.com", "appspot.com", "web.app", "firebaseapp.com", "azurewebsites.net", "cloudfront.net"};
    const std::string last_two = std::string(labels[1]) + "." + std::string(labels[0]);
    if (n >= 3 && std::any_of(std::begin(kShared), std::end(kShared), [&](const char* l) { return last_two == l; })) keep = 3;
    if (n <= keep) return host;
    size_t from = host.size();
    for (size_t k = 0; k < keep; ++k) from = host.rfind('.', from - 1);
    return host.substr(from + 1);
}

// The site of an http(s) address webUrlAllowed() let through; empty for anything else.
inline std::string webSite(std::string_view url) {
    size_t at = url.find("://");
    if (at == std::string_view::npos) return {};
    std::string scheme(url.substr(0, at));
    for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (scheme != "http" && scheme != "https") return {};
    std::string_view authority = url.substr(at + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (authority.find_first_of("@[]") != std::string_view::npos) return {};
    return registrableDomain(authority.substr(0, authority.find(':')));
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
