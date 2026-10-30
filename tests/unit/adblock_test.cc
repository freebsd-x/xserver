// Pure date formatting for AdBlock's list_updated_ label.

#include "xserver/adblock.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::adblock;

TEST(AdBlock, FormatsEpochUtc) {
    EXPECT_EQ(formatListDate(0), "Jan 01 1970");
}

TEST(AdBlock, FormatsKnownDate) {
    // 2026-01-14 00:00:00 UTC = 1768348800.
    EXPECT_EQ(formatListDate(1768348800), "Jan 14 2026");
}

TEST(AdBlock, ProducesFixedWidthLabel) {
    // "%b %d %Y" is always 11 chars → fits list_updated_[24] comfortably.
    EXPECT_EQ(formatListDate(1768348800).size(), 11u);
}

// ── Domain validation ────────────────────────────────────────────────────────

TEST(AdBlock, ValidDomains) {
    EXPECT_TRUE(validDomain("ads.example.com"));
    EXPECT_TRUE(validDomain("a-b.c0.io"));
    EXPECT_FALSE(validDomain("example"));            // one label
    EXPECT_FALSE(validDomain("1.2.3.4"));            // an address, not a name
    EXPECT_FALSE(validDomain("-bad.com"));
    EXPECT_FALSE(validDomain("bad-.com"));
    EXPECT_FALSE(validDomain("a..com"));
    EXPECT_FALSE(validDomain(".a.com"));
    EXPECT_FALSE(validDomain("Ads.com"));            // callers lowercase first
    EXPECT_FALSE(validDomain("a b.com"));
    EXPECT_FALSE(validDomain("a.com\n1.2.3.4 bank.com"));
    EXPECT_FALSE(validDomain(std::string(64, 'a') + ".com"));
}

TEST(AdBlock, NeverBlocksSystemNames) {
    EXPECT_TRUE(neverBlock("localhost"));
    EXPECT_TRUE(neverBlock("pkg.freebsd.org"));
    EXPECT_TRUE(neverBlock("freebsd.org"));
    EXPECT_TRUE(neverBlock("0.pool.ntp.org"));
    EXPECT_FALSE(neverBlock("notfreebsd.org"));
    EXPECT_FALSE(neverBlock("ads.example.com"));
}

// ── List parsing ─────────────────────────────────────────────────────────────

TEST(AdBlock, ParsesHostsFormatOnlySinkholeLines) {
    auto d = parseList(
        "# comment\n"
        "127.0.0.1 localhost\n"                  // never blocked
        "0.0.0.0 Ads.Example.com # trailing\r\n"
        "0.0.0.0 tracker.net\n"
        "tracker.net\n"                          // duplicate, bare form
        "1.2.3.4 bank.com\n"                     // redirect: refused
        "0.0.0.0 a.com b.com\n"                  // two names: skipped
        "0.0.0.0 pkg.freebsd.org\n"              // never blocked
        "::1 ip6.example.org\n"
        "garbage line here\n");
    std::vector<std::string> want{"ads.example.com", "ip6.example.org", "tracker.net"};
    EXPECT_EQ(d, want);
    EXPECT_EQ(formatList(d), "ads.example.com\nip6.example.org\ntracker.net\n");
    EXPECT_EQ(parseList(formatList(d)), d);      // the stored form round-trips
}

// ── /etc/hosts splicing ──────────────────────────────────────────────────────

TEST(AdBlock, SplicesOnlyItsOwnBlock) {
    const std::string base = "127.0.0.1 localhost\n192.168.1.5 nas.lan\n";
    std::string on = spliceHosts(base, {"ads.example.com", "tracker.net"});
    EXPECT_EQ(on.substr(0, base.size()), base);
    EXPECT_NE(on.find("0.0.0.0 ads.example.com\n"), std::string::npos);
    EXPECT_EQ(blockedCount(on), 2u);

    // A user edit after our block survives a refresh; the block is replaced.
    std::string edited = on + "10.0.0.2 printer.lan\n";
    std::string refreshed = spliceHosts(edited, {"x.example.com"});
    EXPECT_EQ(blockedCount(refreshed), 1u);
    EXPECT_NE(refreshed.find("10.0.0.2 printer.lan\n"), std::string::npos);
    EXPECT_EQ(refreshed.find("tracker.net"), std::string::npos);

    // Turning it off restores the user's content exactly.
    EXPECT_EQ(spliceHosts(on, {}), base);
    EXPECT_EQ(blockedCount(base), 0u);
}

TEST(AdBlock, UnterminatedBlockIsLeftAlone) {
    const std::string hosts = "127.0.0.1 localhost\n" + std::string(kBlockBegin) +
                              "\n0.0.0.0 a.com\n192.168.1.5 nas.lan\n";
    EXPECT_EQ(spliceHosts(hosts, {}), hosts);    // nothing after BEGIN is lost
}

TEST(AdBlock, NoTrailingNewlineIsHandled) {
    std::string on = spliceHosts("127.0.0.1 localhost", {"ads.example.com"});
    EXPECT_EQ(on.find("127.0.0.1 localhost\n"), 0u);
    EXPECT_EQ(blockedCount(on), 1u);
}
