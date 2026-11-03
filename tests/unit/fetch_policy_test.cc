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
