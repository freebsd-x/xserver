// Unit tests for DNS + Proxy config helpers (pure logic).

#include "xserver/net_config.h"
#include <gtest/gtest.h>
#include <cstring>

using namespace fbx::xserver::netcfg;

// ── firstNameserver ───────────────────────────────────────────────────────────

TEST(NetConfig, FirstNameserver) {
    EXPECT_EQ(firstNameserver("# comment\nnameserver 192.168.1.1\nnameserver 8.8.8.8\n"),
              "192.168.1.1");
    EXPECT_EQ(firstNameserver("  nameserver\t10.0.0.1  \n"), "10.0.0.1");  // tabs/ws
    EXPECT_EQ(firstNameserver("search lan\noptions edns0\n"), "");        // none
    EXPECT_EQ(firstNameserver(""), "");
}

// ── buildDnsConfig ────────────────────────────────────────────────────────────

TEST(NetConfig, DnsAutoLabelsResolver) {
    auto c = buildDnsConfig(kDnsAuto, "", "192.168.1.1");
    EXPECT_EQ(c.mode_, kDnsAuto);
    EXPECT_STREQ(c.resolver_, "192.168.1.1 (DHCP)");
    EXPECT_STREQ(c.doh_url_, "");
}

TEST(NetConfig, DnsAutoNoResolver) {
    auto c = buildDnsConfig(kDnsAuto, "", "");
    EXPECT_STREQ(c.resolver_, "Automatic (DHCP)");
}

TEST(NetConfig, DnsDoH) {
    auto c = buildDnsConfig(kDnsDoH, "", "192.168.1.1");
    EXPECT_EQ(c.mode_, kDnsDoH);
    EXPECT_STREQ(c.resolver_, "Cloudflare (DoH)");
    EXPECT_STREQ(c.doh_url_, "https://1.1.1.1/dns-query");
}

TEST(NetConfig, DnsCustom) {
    auto c = buildDnsConfig(kDnsCustom, "9.9.9.9", "192.168.1.1");
    EXPECT_EQ(c.mode_, kDnsCustom);
    EXPECT_STREQ(c.resolver_, "9.9.9.9");
    EXPECT_STREQ(c.custom_ip_, "9.9.9.9");
}

// ── resolvConfFor ─────────────────────────────────────────────────────────────

TEST(NetConfig, ResolvConfBodies) {
    EXPECT_EQ(resolvConfFor(kDnsAuto, ""), "");                        // leave DHCP alone
    EXPECT_NE(resolvConfFor(kDnsCustom, "9.9.9.9").find("nameserver 9.9.9.9"),
              std::string::npos);
    EXPECT_NE(resolvConfFor(kDnsDoH, "").find("nameserver 127.0.0.1"),
              std::string::npos);
    EXPECT_EQ(resolvConfFor(kDnsCustom, ""), "");                      // custom w/o ip → noop
}

// ── Proxy round-trip ──────────────────────────────────────────────────────────

TEST(NetConfig, ProxyRoundTrip) {
    fbx::shared::ProxyConfig p{};
    p.mode_ = 2;
    std::strcpy(p.http_host_, "proxy.corp");
    p.http_port_  = 8080;
    p.https_port_ = 8443;
    std::strcpy(p.no_proxy_, "localhost,10.0.0.0/8");
    std::strcpy(p.pac_url_, "http://wpad/wpad.dat");

    fbx::shared::ProxyConfig r = parseProxy(serializeProxy(p));
    EXPECT_EQ(r.mode_, 2u);
    EXPECT_STREQ(r.http_host_, "proxy.corp");
    EXPECT_EQ(r.http_port_, 8080);
    EXPECT_EQ(r.https_port_, 8443);
    EXPECT_STREQ(r.no_proxy_, "localhost,10.0.0.0/8");
    EXPECT_STREQ(r.pac_url_, "http://wpad/wpad.dat");
}

TEST(NetConfig, ProxyParseDefaultsAndIgnoresJunk) {
    auto p = parseProxy("mode=1\ngarbage line\n\nhttp_port=3128\n");
    EXPECT_EQ(p.mode_, 1u);
    EXPECT_EQ(p.http_port_, 3128);
    EXPECT_STREQ(p.http_host_, "");   // missing key → zero default
}
