// Unit tests for sockstat parsing + normalisation (pure logic).

#include "xserver/net_conn.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::net;

// ── normProto ─────────────────────────────────────────────────────────────────

TEST(NetConn, NormProto) {
    EXPECT_EQ(normProto("tcp4"), "TCP");
    EXPECT_EQ(normProto("tcp6"), "TCP");
    EXPECT_EQ(normProto("udp4"), "UDP");
    EXPECT_EQ(normProto("UDP6"), "UDP");
}

// ── abbrevState ───────────────────────────────────────────────────────────────

TEST(NetConn, AbbrevState) {
    EXPECT_EQ(abbrevState("ESTABLISHED"), "ESTABL");
    EXPECT_EQ(abbrevState("TIME_WAIT"), "TIMEWT");
    EXPECT_EQ(abbrevState("CLOSE_WAIT"), "CLOSWT");
    EXPECT_EQ(abbrevState("LISTEN"), "LISTEN");
    EXPECT_LE(abbrevState("SOME_LONG_STATE").size(), 6u);
}

// ── connId ────────────────────────────────────────────────────────────────────

TEST(NetConn, ConnIdStableAndDistinct) {
    uint32_t a = connId("TCP", "1.2.3.4:22", "5.6.7.8:9999");
    EXPECT_EQ(a, connId("TCP", "1.2.3.4:22", "5.6.7.8:9999"));      // stable
    EXPECT_NE(a, connId("TCP", "1.2.3.4:22", "5.6.7.8:9998"));      // remote port
    EXPECT_NE(a, connId("UDP", "1.2.3.4:22", "5.6.7.8:9999"));      // proto
    EXPECT_NE(0u, a);
}

// ── parseSockstat ─────────────────────────────────────────────────────────────

static const char* kSample =
    "USER     COMMAND    PID   FD PROTO  LOCAL ADDRESS       FOREIGN ADDRESS      STATE\n"
    "root     sshd      1234    4 tcp4   192.168.1.10:22     192.168.1.5:52341    ESTABLISHED\n"
    "www      nginx      900    6 tcp4   *:80                *:*                  LISTEN\n"
    "bob      ntpd       55     3 udp4   0.0.0.0:123         *:*\n"
    "root     init        1    10 stream /var/run/foo.sock   -\n";  // UNIX — skipped

TEST(NetConn, ParsesEstablishedTcp) {
    auto v = parseSockstat(kSample);
    ASSERT_GE(v.size(), 1u);
    EXPECT_EQ(v[0].command, "sshd");
    EXPECT_EQ(v[0].pid, 1234u);
    EXPECT_EQ(v[0].proto, "TCP");
    EXPECT_EQ(v[0].local, "192.168.1.10:22");
    EXPECT_EQ(v[0].remote, "192.168.1.5:52341");
    EXPECT_EQ(v[0].state, "ESTABLISHED");
}

TEST(NetConn, ListeningSocketRemoteDash) {
    auto v = parseSockstat(kSample);
    ASSERT_GE(v.size(), 2u);
    EXPECT_EQ(v[1].command, "nginx");
    EXPECT_EQ(v[1].remote, "-");
    EXPECT_EQ(v[1].state, "LISTEN");
}

TEST(NetConn, UdpWithoutStateInfersListen) {
    auto v = parseSockstat(kSample);
    ASSERT_GE(v.size(), 3u);
    EXPECT_EQ(v[2].proto, "UDP");
    EXPECT_EQ(v[2].remote, "-");
    EXPECT_EQ(v[2].state, "LISTEN");   // "*:*" foreign, no state column
}

TEST(NetConn, SkipsUnixAndHeader) {
    auto v = parseSockstat(kSample);
    EXPECT_EQ(v.size(), 3u);           // header + UNIX row excluded
    for (const auto& c : v)
        EXPECT_TRUE(c.proto == "TCP" || c.proto == "UDP");
}

TEST(NetConn, EmptyInput) {
    EXPECT_TRUE(parseSockstat("").empty());
    EXPECT_TRUE(parseSockstat("USER COMMAND PID FD PROTO LOCAL FOREIGN\n").empty());
}
