// Pure resolver-field validation + resolv.conf body for DnsConfig.

#include "xserver/dns_resolver.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::dnsresolver;

TEST(DnsResolver, ValidFields) {
    EXPECT_TRUE(validField(""));                  // empty secondary ok
    EXPECT_TRUE(validField("1.1.1.1"));
    EXPECT_TRUE(validField("2606:4700:4700::1111"));   // IPv6
    EXPECT_TRUE(validField("dns.example.com"));
}

TEST(DnsResolver, RejectsInjection) {
    EXPECT_FALSE(validField("1.1.1.1\nnameserver 9.9.9.9"));  // newline injection
    EXPECT_FALSE(validField("1.1.1.1 extra"));                // space
    EXPECT_FALSE(validField("1.1.1.1;reboot"));               // shell-ish
}

TEST(DnsResolver, ResolvConfBodyPrimaryOnly) {
    EXPECT_EQ(resolvConfBody("1.1.1.1", ""),
              "# written by com.freebsdx.DnsConfig\nnameserver 1.1.1.1\n");
}

TEST(DnsResolver, ResolvConfBodyBoth) {
    EXPECT_EQ(resolvConfBody("1.1.1.1", "8.8.8.8"),
              "# written by com.freebsdx.DnsConfig\n"
              "nameserver 1.1.1.1\nnameserver 8.8.8.8\n");
}

TEST(DnsResolver, ResolvConfBodyEmptyWhenNoPrimary) {
    EXPECT_EQ(resolvConfBody("", "8.8.8.8"),
              "# written by com.freebsdx.DnsConfig\nnameserver 8.8.8.8\n");
    EXPECT_EQ(resolvConfBody("", ""), "");        // nothing to write
}
