// Pure parsers for the system-admin services (Services/Jails/ZfsPools/BootEnv/NTP).
// Inputs are real output captured from this FreeBSD 15 box.

#include "xserver/sysadmin_parse.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::sysadmin;

// ── rc scripts ───────────────────────────────────────────────────────────────

TEST(SysadminRc, DaemonWithCommandNameExpansion) {
    RcDaemon d;
    ASSERT_TRUE(parseRcScript("sshd",
        "name=\"sshd\"\nrcvar=\"sshd_enable\"\ncommand=\"/usr/sbin/${name}\"\n"
        "pidfile=\"/var/run/${name}.pid\"\n", d));
    EXPECT_EQ(d.name, "sshd");
    EXPECT_EQ(d.procname, "sshd");
}

TEST(SysadminRc, ProcnameWinsOverDaemon8Wrapper) {
    // seatd/xsessiond run under daemon(8): command is the wrapper, procname the real binary.
    RcDaemon d;
    ASSERT_TRUE(parseRcScript("seatd",
        "name=seatd\ncommand=\"/usr/sbin/daemon\"\nprocname=\"/usr/local/bin/${name}\"\n"
        "pidfile=\"/var/run/${name}.pid\"\n", d));
    EXPECT_EQ(d.procname, "seatd");
}

TEST(SysadminRc, LiteralCommandAndUnquotedName) {
    RcDaemon d;
    ASSERT_TRUE(parseRcScript("dbus",
        "name=dbus\ncommand=\"/usr/local/bin/dbus-daemon\"\npidfile=\"/var/run/dbus/pid\"\n", d));
    EXPECT_EQ(d.procname, "dbus-daemon");
}

TEST(SysadminRc, OneShotScriptsAreNotDaemons) {
    RcDaemon d;
    // newsyslog has command= but no pidfile=; hostid has neither.
    EXPECT_FALSE(parseRcScript("newsyslog", "name=\"newsyslog\"\ncommand=\"/usr/sbin/${name}\"\n", d));
    EXPECT_FALSE(parseRcScript("hostid", "name=\"hostid\"\nstart_cmd=\"hostid_start\"\n", d));
}

TEST(SysadminRc, UnexpandableVariableRejected) {
    RcDaemon d;
    EXPECT_FALSE(parseRcScript("x", "name=x\ncommand=\"${x_prefix}/bin/x\"\npidfile=/var/run/x.pid\n", d));
}

TEST(SysadminRc, CommMatchesTruncatedKernelName) {
    EXPECT_TRUE(commMatches("sshd", "sshd"));
    EXPECT_FALSE(commMatches("sshd", "ssh"));
    // ki_comm holds at most 19 chars.
    EXPECT_TRUE(commMatches("a-very-long-daemon-", "a-very-long-daemon-name"));
}

// ── zpool ────────────────────────────────────────────────────────────────────

TEST(SysadminZfs, ParsesZpoolList) {
    auto p = parseZpoolList("zroot\t253403070464\t192021884928\tONLINE\n"
                            "tank\t1000\t10\tDEGRADED\n");
    ASSERT_EQ(p.size(), 2u);
    EXPECT_EQ(p[0].name, "zroot");
    EXPECT_EQ(p[0].size_bytes, 253403070464ull);
    EXPECT_EQ(p[0].alloc_bytes, 192021884928ull);
    EXPECT_EQ(p[0].health, "ONLINE");
    EXPECT_EQ(p[1].health, "DEGRADED");
    EXPECT_TRUE(parseZpoolList("").empty());
}

TEST(SysadminZfs, ScrubProgress) {
    uint32_t pct = 99;
    EXPECT_FALSE(parseScrubProgress("  pool: zroot\n state: ONLINE\n", pct));
    EXPECT_EQ(pct, 0u);
    EXPECT_FALSE(parseScrubProgress(
        "  scan: scrub repaired 0B in 00:10:00 with 0 errors on Sun Jul 25\n", pct));
    EXPECT_TRUE(parseScrubProgress(
        "  scan: scrub in progress since Sun Jul 25 10:00:00 2026\n"
        "\t1.2G / 10G scanned at 100M/s, 456M / 10G issued, 45.67% done, 00:10:00 to go\n", pct));
    EXPECT_EQ(pct, 45u);
}

// ── bectl ────────────────────────────────────────────────────────────────────

TEST(SysadminBe, ParsesActiveAndNextBootFlags) {
    auto b = parseBectlList(
        "15.1-RELEASE-p3_2026-10-26_213713\t-\t-\t190M\t2026-10-26 21:37\n"
        "default\tNR\t/\t60.1G\t2023-11-23 21:08\n"
        "other\tR\t-\t1M\t2024-01-01 00:00\n");
    ASSERT_EQ(b.size(), 3u);
    EXPECT_EQ(b[0].name, "15.1-RELEASE-p3_2026-10-26_213713");
    EXPECT_FALSE(b[0].active);
    EXPECT_FALSE(b[0].next_boot);
    EXPECT_EQ(b[0].created, "2026-10-26 21:37");
    EXPECT_TRUE(b[1].active);
    EXPECT_TRUE(b[1].next_boot);
    EXPECT_FALSE(b[2].active);
    EXPECT_TRUE(b[2].next_boot);
}

// ── jls ──────────────────────────────────────────────────────────────────────

TEST(SysadminJails, ParsesKeyValueRows) {
    auto j = parseJls("jid=3 host.hostname=web ip4.addr=10.0.0.3,10.0.0.4 dying=false\n"
                      "jid=7 host.hostname=db ip4.addr=- dying=true\n");
    ASSERT_EQ(j.size(), 2u);
    EXPECT_EQ(j[0].jid, 3u);
    EXPECT_EQ(j[0].hostname, "web");
    EXPECT_EQ(j[0].ip, "10.0.0.3");          // first of the comma list
    EXPECT_FALSE(j[0].dying);
    EXPECT_TRUE(j[1].dying);
    EXPECT_TRUE(parseJls("").empty());       // no jails running → empty output
}

// ── ntpq ─────────────────────────────────────────────────────────────────────

TEST(SysadminNtp, UnsyncedUsesFirstPeer) {
    auto n = parseNtpqPeers(
        "     remote           refid      st t when poll reach   delay   offset  jitter\n"
        "==============================================================================\n"
        " 0.freebsd.pool. .POOL.          16 p    -   64    0    0.000   +0.000   0.000\n"
        "-121.174.142.82  220.73.142.66    3 u  111  128  377    9.608   +1.406   0.374\n");
    EXPECT_FALSE(n.synced);
    EXPECT_EQ(n.server, "0.freebsd.pool.");
}

TEST(SysadminNtp, SyncedPeerOffsetAndWhenSuffix) {
    auto n = parseNtpqPeers(
        "     remote           refid      st t when poll reach   delay   offset  jitter\n"
        "==============================================================================\n"
        " 0.freebsd.pool. .POOL.          16 p    -   64    0    0.000   +0.000   0.000\n"
        "*121.174.142.82  220.73.142.66    3 u   2m  128  377    9.608   -12.6   0.374\n");
    EXPECT_TRUE(n.synced);
    EXPECT_EQ(n.server, "121.174.142.82");
    EXPECT_NEAR(n.offset_ms, -12.6, 1e-9);
    EXPECT_EQ(n.when_sec, 120u);
}

TEST(SysadminNtp, EmptyOutput) {
    auto n = parseNtpqPeers("");
    EXPECT_FALSE(n.synced);
    EXPECT_TRUE(n.server.empty());
}

// ── sshd ─────────────────────────────────────────────────────────────────────

TEST(SysadminSshd, DefaultsWhenAllCommentedOut) {
    auto c = parseSshdConfig("#Port 22\n#ListenAddress 0.0.0.0\nPermitRootLogin no\n");
    EXPECT_EQ(c.port, 22u);
    EXPECT_EQ(c.listen, "0.0.0.0");
}

TEST(SysadminSshd, FirstValueWinsAndMatchEndsGlobals) {
    auto c = parseSshdConfig("port 2222\nPort 22\nListenAddress 192.168.0.3:2222\n"
                             "Match User git\n  Port 9999\n");
    EXPECT_EQ(c.port, 2222u);
    EXPECT_EQ(c.listen, "192.168.0.3");
    EXPECT_EQ(parseSshdConfig("ListenAddress [::1]:22\n").listen, "::1");
    EXPECT_EQ(parseSshdConfig("ListenAddress ::\n").listen, "::");
    EXPECT_EQ(parseSshdConfig("Port 70000\n").port, 22u);   // invalid → default
}

TEST(SysadminSshd, CountsInboundEstablishedOnly) {
    constexpr std::string_view kNetstat =
        "Active Internet connections (including servers)\n"
        "Proto     Recv-Q Send-Q Local Address          Foreign Address        (state)\n"
        "tcp4           0      0 192.168.0.3.22         192.168.0.9.51234      ESTABLISHED\n"
        "tcp6           0      0 fe80::1%em0.22         fe80::2%em0.40000      ESTABLISHED\n"
        "tcp4           0      0 192.168.0.3.47060      160.79.104.10.22       ESTABLISHED\n"
        "tcp4           0      0 192.168.0.3.222        10.0.0.1.5000          ESTABLISHED\n"
        "tcp4           0      0 *.22                   *.*                    LISTEN\n";
    EXPECT_EQ(countEstablished(kNetstat, 22), 2u);   // outbound-to-22 and port 222 excluded
    EXPECT_EQ(countEstablished("", 22), 0u);
}

// ── pkg ──────────────────────────────────────────────────────────────────────

TEST(SysadminPkg, CountsOutdatedPackages) {
    auto u = parsePkgOutdated(
        "firefox-131.0_1                    <   needs updating (remote has 132.0,2)\n"
        "Imath-3.2.3_1                      =   up-to-date with remote\n"
        "py311-pip-24.0                     <   needs updating (remote has 24.2)\n");
    EXPECT_EQ(u.count, 2u);
    EXPECT_EQ(u.first, "firefox 132.0,2");
    EXPECT_EQ(parsePkgOutdated("").count, 0u);
    EXPECT_EQ(parsePkgOutdated("").first, "");
}

TEST(SysadminWlan, OnlyWifiInterfaces) {
    auto v = parseWlanIfaces("lo0 em0 wlan0 wlan12 wlanx bridge0\n");
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0], "wlan0");
    EXPECT_EQ(v[1], "wlan12");
    EXPECT_TRUE(parseWlanIfaces("").empty());
}
