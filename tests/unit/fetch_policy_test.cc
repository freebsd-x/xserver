// Unit tests for fetch_policy.h — which hosts each app may reach through
// com.freebsdx.Fetch, and the URL parsing that decides it.

#include "xserver/fetch_policy.h"

#include <gtest/gtest.h>

namespace fp = fbx::xserver::fetch_policy;

TEST(FetchPolicy, MapsReachesItsHosts) {
    EXPECT_TRUE(fp::allowed("com.freebsdx.maps", "https://tile.openstreetmap.org/12/2076/1410.png"));
    EXPECT_TRUE(fp::allowed("com.freebsdx.maps",
                            "https://nominatim.openstreetmap.org/search?q=Paris&format=json&limit=1"));
    EXPECT_TRUE(fp::allowed("com.freebsdx.maps", "http://ip-api.com/json"));
    EXPECT_TRUE(fp::allowed("com.freebsdx.maps", "HTTPS://TILE.OpenStreetMap.org/1/0/0.png"));
}

TEST(FetchPolicy, OtherAppsAndUnknownCallersAreRefused) {
    EXPECT_FALSE(fp::allowed("com.freebsdx.weather", "https://tile.openstreetmap.org/1/0/0.png"));
    EXPECT_FALSE(fp::allowed("", "https://tile.openstreetmap.org/1/0/0.png"));
}

TEST(FetchPolicy, OnlyTheExactHost) {
    const char* app = "com.freebsdx.maps";
    EXPECT_FALSE(fp::allowed(app, "https://example.com/"));
    EXPECT_FALSE(fp::allowed(app, "https://tile.openstreetmap.org.evil.example/1/0/0.png"));
    EXPECT_FALSE(fp::allowed(app, "https://evil.example/tile.openstreetmap.org/1/0/0.png"));
    EXPECT_FALSE(fp::allowed(app, "https://x.tile.openstreetmap.org/1/0/0.png"));
}

TEST(FetchPolicy, NoUserinfoPortsOrOddCharacters) {
    const char* app = "com.freebsdx.maps";
    EXPECT_FALSE(fp::allowed(app, "https://tile.openstreetmap.org@evil.example/"));
    EXPECT_FALSE(fp::allowed(app, "https://evil.example@tile.openstreetmap.org/"));
    EXPECT_FALSE(fp::allowed(app, "https://tile.openstreetmap.org:8443/1/0/0.png"));
    EXPECT_FALSE(fp::allowed(app, "https://tile.openstreetmap.org/a b"));
    EXPECT_FALSE(fp::allowed(app, "https://tile.openstreetmap.org\\@evil.example/"));
    EXPECT_FALSE(fp::allowed(app, "https://tile.openstreetmap.org/\n"));
    EXPECT_FALSE(fp::allowed(app, "https://[::1]/"));
}

TEST(FetchPolicy, SchemeRules) {
    const char* app = "com.freebsdx.maps";
    EXPECT_FALSE(fp::allowed(app, "http://tile.openstreetmap.org/1/0/0.png"));   // https only
    EXPECT_FALSE(fp::allowed(app, "file:///etc/passwd"));
    EXPECT_FALSE(fp::allowed(app, "ftp://tile.openstreetmap.org/"));
    EXPECT_FALSE(fp::allowed(app, "tile.openstreetmap.org/1/0/0.png"));
    EXPECT_FALSE(fp::allowed(app, std::string("https://tile.openstreetmap.org/") + std::string(300, 'a')));
}

TEST(FetchPolicy, KnownHostAndCacheability) {
    bool cacheable = false;
    EXPECT_TRUE(fp::knownHost("https://tile.openstreetmap.org/1/0/0.png", &cacheable));
    EXPECT_TRUE(cacheable);
    EXPECT_TRUE(fp::knownHost("https://nominatim.openstreetmap.org/search?q=x", &cacheable));
    EXPECT_FALSE(cacheable);   // search results are never cached
    EXPECT_FALSE(fp::knownHost("https://example.com/"));
}

TEST(FetchPolicy, CompositorServicesHaveTheirOwnHosts) {
    const auto comp = fp::kCompositor;
    EXPECT_TRUE(fp::allowed(comp, "https://wttr.in/Paris?format=j1"));
    EXPECT_TRUE(fp::allowed(comp, "https://pkg.freebsd-x.org/catalog.json"));
    EXPECT_TRUE(fp::allowed(comp, "https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts"));
    // Packages may not come from just anywhere any more.
    EXPECT_FALSE(fp::allowed(comp, "https://evil.example/app.xpkg"));
    EXPECT_FALSE(fp::allowed(comp, "http://pkg.freebsd-x.org/catalog.json"));   // https only
    // ...and an app can't reach the compositor's hosts.
    EXPECT_FALSE(fp::allowed("com.freebsdx.maps", "https://wttr.in/Paris"));
    EXPECT_FALSE(fp::allowed("com.freebsdx.maps", "https://pkg.freebsd-x.org/x.xpkg"));
}

TEST(FetchPolicy, SizeLimitsFollowTheRule) {
    size_t limit = 0;
    bool cacheable = true;
    ASSERT_TRUE(fp::knownHost("https://pkg.freebsd-x.org/big.xpkg", &cacheable, &limit));
    EXPECT_EQ(limit, 256 * fp::kMiB);   // packages are large
    EXPECT_FALSE(cacheable);
    ASSERT_TRUE(fp::knownHost("https://tile.openstreetmap.org/1/0/0.png", &cacheable, &limit));
    EXPECT_EQ(limit, 8 * fp::kMiB);
    EXPECT_TRUE(cacheable);
}

namespace {
bool privateV4(const char* text) {
    struct sockaddr_in a{};
    a.sin_family = AF_INET;
    inet_pton(AF_INET, text, &a.sin_addr);
    return fbx::xserver::fetch_policy::isPrivateAddress(reinterpret_cast<const struct sockaddr*>(&a));
}
bool privateV6(const char* text) {
    struct sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    inet_pton(AF_INET6, text, &a.sin6_addr);
    return fbx::xserver::fetch_policy::isPrivateAddress(reinterpret_cast<const struct sockaddr*>(&a));
}
}  // namespace

TEST(WebPolicy, UrlsABrowserMayAskFor) {
    using fbx::xserver::fetch_policy::webUrlAllowed;
    EXPECT_TRUE(webUrlAllowed("https://example.com/"));
    EXPECT_TRUE(webUrlAllowed("HTTP://example.com"));
    EXPECT_TRUE(webUrlAllowed("https://example.com:8443/a?b=c#d"));
    EXPECT_TRUE(webUrlAllowed("http://example.com:80/"));
    EXPECT_FALSE(webUrlAllowed("file:///etc/passwd"));
    EXPECT_FALSE(webUrlAllowed("ftp://example.com/"));
    EXPECT_FALSE(webUrlAllowed("javascript:alert(1)"));
    EXPECT_FALSE(webUrlAllowed("https://user:pw@example.com/"));
    EXPECT_FALSE(webUrlAllowed("https://example.com@evil.test/"));
    EXPECT_FALSE(webUrlAllowed("http://example.com:25/"));     // not the web's ports
    EXPECT_FALSE(webUrlAllowed("http://example.com:22/"));
    EXPECT_FALSE(webUrlAllowed("http://example.com:99999/"));
    EXPECT_FALSE(webUrlAllowed("http://example.com:x/"));
    EXPECT_FALSE(webUrlAllowed("http://[::1]/"));
    EXPECT_FALSE(webUrlAllowed("http:///nohost"));
    EXPECT_FALSE(webUrlAllowed("http://a b/"));
    EXPECT_FALSE(webUrlAllowed("http://a\\b/"));
    EXPECT_TRUE(webUrlAllowed("https://example.com/" + std::string(1000, 'a')));
    EXPECT_FALSE(webUrlAllowed("https://example.com/" + std::string(9000, 'a')));
}

TEST(WebPolicy, ReachingForTheLocalNetworkIsRefused) {
    for (const char* a : {"127.0.0.1", "127.1.2.3", "10.0.0.1", "192.168.0.1", "172.16.0.1", "172.31.255.255", "169.254.169.254",
                          "0.0.0.0", "100.64.0.1", "224.0.0.1", "255.255.255.255"})
        EXPECT_TRUE(privateV4(a)) << a;
    for (const char* a : {"1.1.1.1", "8.8.8.8", "172.32.0.1", "172.15.0.1", "192.167.0.1", "100.128.0.1", "93.184.216.34"})
        EXPECT_FALSE(privateV4(a)) << a;
    for (const char* a : {"::1", "::", "fe80::1", "fc00::1", "fd12::1", "ff02::1", "::ffff:127.0.0.1", "::ffff:192.168.1.1"})
        EXPECT_TRUE(privateV6(a)) << a;
    for (const char* a : {"2001:4860:4860::8888", "::ffff:8.8.8.8"}) EXPECT_FALSE(privateV6(a)) << a;
}

TEST(WebPolicy, OtherAppsKeepTheirAllowlists) {
    using namespace fbx::xserver::fetch_policy;
    EXPECT_FALSE(allowed("com.freebsdx.maps", "https://example.com/"));
    EXPECT_FALSE(allowed(kWebApp, "https://example.com/"));   // only the web flag's path, which checks who asks
    EXPECT_FALSE(knownHost("https://example.com/"));
}
