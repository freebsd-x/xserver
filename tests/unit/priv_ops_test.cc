// The privilege broker's allowlist + argument validator — the security boundary.
// These cases ARE the security property: only known verbs with safe args pass.

#include "xserver/priv_ops.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::privops;

TEST(PrivOps, KnownVerbs) {
    EXPECT_TRUE(knownVerb(kResolvConfWrite));
    EXPECT_TRUE(knownVerb(kAudioDefaultUnit));
    EXPECT_FALSE(knownVerb("resolvconf.read"));
    EXPECT_FALSE(knownVerb("exec"));
    EXPECT_FALSE(knownVerb(""));
}

TEST(PrivOps, UnknownVerbAlwaysRejected) {
    EXPECT_FALSE(validateOp("reboot", ""));
    EXPECT_FALSE(validateOp("rm", "-rf /"));
    EXPECT_FALSE(validateOp("", "anything"));
}

// ── resolvconf.write ──────────────────────────────────────────────────────────

TEST(PrivOps, ResolvConfAcceptsNameserverLines) {
    EXPECT_TRUE(validateOp(kResolvConfWrite, "nameserver 1.1.1.1\n"));
    EXPECT_TRUE(validateOp(kResolvConfWrite,
                           "# written by com.freebsdx.DnsConfig\n"
                           "nameserver 1.1.1.1\nnameserver 8.8.8.8\n"));
    EXPECT_TRUE(validateOp(kResolvConfWrite, "nameserver 2606:4700:4700::1111\n"));
}

TEST(PrivOps, ResolvConfRejectsNonNameserverDirectives) {
    // Only comments + nameserver lines allowed — no options/search/etc.
    EXPECT_FALSE(validateOp(kResolvConfWrite, "options rotate\n"));
    EXPECT_FALSE(validateOp(kResolvConfWrite, "search evil.example\n"));
    EXPECT_FALSE(validateOp(kResolvConfWrite, "nameserver 1.1.1.1\noptions ndots:5\n"));
}

TEST(PrivOps, ResolvConfRejectsInjectionAndJunk) {
    EXPECT_FALSE(validateOp(kResolvConfWrite, "nameserver 1.1.1.1; rm -rf /\n"));  // space+junk
    EXPECT_FALSE(validateOp(kResolvConfWrite, "nameserver \n"));                    // empty field
    EXPECT_FALSE(validateOp(kResolvConfWrite, ""));                                 // empty body
    EXPECT_FALSE(validateOp(kResolvConfWrite, std::string(5000, 'x')));             // oversized
}

// ── audio.default_unit ──────────────────────────────────────────────────────────

TEST(PrivOps, AudioUnitAcceptsSmallIntegers) {
    EXPECT_TRUE(validateOp(kAudioDefaultUnit, "0"));
    EXPECT_TRUE(validateOp(kAudioDefaultUnit, "1"));
    EXPECT_TRUE(validateOp(kAudioDefaultUnit, "63"));
    EXPECT_TRUE(validateOp(kAudioDefaultUnit, "999"));
}

TEST(PrivOps, AudioUnitRejectsNonNumericOrOversized) {
    EXPECT_FALSE(validateOp(kAudioDefaultUnit, ""));
    EXPECT_FALSE(validateOp(kAudioDefaultUnit, "1a"));
    EXPECT_FALSE(validateOp(kAudioDefaultUnit, "-1"));
    EXPECT_FALSE(validateOp(kAudioDefaultUnit, "1000"));     // 4 digits
    EXPECT_FALSE(validateOp(kAudioDefaultUnit, "1;reboot"));
}

// ── firewall.enable ──────────────────────────────────────────────────────────

TEST(PrivOps, FirewallEnableAcceptsOnlyZeroOrOne) {
    EXPECT_TRUE(knownVerb(kFirewallEnable));
    EXPECT_TRUE(validateOp(kFirewallEnable, "0"));
    EXPECT_TRUE(validateOp(kFirewallEnable, "1"));
}

TEST(PrivOps, FirewallEnableRejectsAnythingElse) {
    EXPECT_FALSE(validateOp(kFirewallEnable, ""));
    EXPECT_FALSE(validateOp(kFirewallEnable, "2"));
    EXPECT_FALSE(validateOp(kFirewallEnable, "-e"));         // can't smuggle a flag
    EXPECT_FALSE(validateOp(kFirewallEnable, "1 -f /etc/evil"));
    EXPECT_FALSE(validateOp(kFirewallEnable, "true"));
}

// ── kmod.unload ──────────────────────────────────────────────────────────────

TEST(PrivOps, KmodUnloadAcceptsModuleNames) {
    EXPECT_TRUE(knownVerb(kKmodUnload));
    EXPECT_TRUE(validateOp(kKmodUnload, "ichsmb.ko"));
    EXPECT_TRUE(validateOp(kKmodUnload, "if_em"));
    EXPECT_TRUE(validateOp(kKmodUnload, "cpufreq"));
}

TEST(PrivOps, KmodUnloadRejectsFlagsAndInjection) {
    EXPECT_FALSE(validateOp(kKmodUnload, ""));
    EXPECT_FALSE(validateOp(kKmodUnload, "-f"));             // can't start with a flag
    EXPECT_FALSE(validateOp(kKmodUnload, "a b"));            // space
    EXPECT_FALSE(validateOp(kKmodUnload, "mod;reboot"));     // shell-ish
    EXPECT_FALSE(validateOp(kKmodUnload, "../../boot/kernel")); // slash
    EXPECT_FALSE(validateOp(kKmodUnload, std::string(65, 'a'))); // too long
}

// ── service.start / service.stop ─────────────────────────────────────────────

TEST(PrivOps, ServiceControlAcceptsRcNames) {
    EXPECT_TRUE(knownVerb(kServiceStart));
    EXPECT_TRUE(knownVerb(kServiceStop));
    EXPECT_TRUE(validateOp(kServiceStart, "sshd"));
    EXPECT_TRUE(validateOp(kServiceStop, "ntpd"));
    EXPECT_TRUE(validateOp(kServiceStop, "hostid_save"));
}

TEST(PrivOps, ServiceControlRejectsSessionCriticalAndInjection) {
    // Stopping these from a panel would kill the desktop session itself.
    EXPECT_FALSE(validateOp(kServiceStop, "xsessiond"));
    EXPECT_FALSE(validateOp(kServiceStop, "seatd"));
    EXPECT_FALSE(validateOp(kServiceStart, "xsessiond"));
    EXPECT_FALSE(validateOp(kServiceStop, ""));
    EXPECT_FALSE(validateOp(kServiceStop, "-j"));             // flag
    EXPECT_FALSE(validateOp(kServiceStop, "sshd onerestart")); // extra argv
    EXPECT_FALSE(validateOp(kServiceStop, "../../bin/sh"));
    EXPECT_FALSE(validateOp(kServiceStop, "sshd.old"));       // no '.' in rc names
    EXPECT_FALSE(validateOp(kServiceStop, std::string(33, 'a')));
}

// ── zfs.scrub / be.activate / be.destroy ─────────────────────────────────────

TEST(PrivOps, ZfsAndBeAcceptRealNames) {
    EXPECT_TRUE(validateOp(kZfsScrub, "zroot"));
    EXPECT_TRUE(validateOp(kBeActivate, "15.1-RELEASE-p3_2026-10-26_213713"));
    EXPECT_TRUE(validateOp(kBeDestroy, "default"));
}

TEST(PrivOps, ZfsAndBeRejectDatasetPathsAndFlags) {
    EXPECT_FALSE(validateOp(kBeDestroy, "zroot/ROOT/default"));  // no dataset paths
    EXPECT_FALSE(validateOp(kBeDestroy, "-F"));
    EXPECT_FALSE(validateOp(kBeDestroy, "default@snap"));
    EXPECT_FALSE(validateOp(kZfsScrub, "-s"));                 // would stop a scrub
    EXPECT_FALSE(validateOp(kZfsScrub, "zroot tank"));
    EXPECT_FALSE(validateOp(kBeActivate, ""));
}

// ── jail.remove ──────────────────────────────────────────────────────────────

TEST(PrivOps, JailRemoveOnlyPositiveJids) {
    EXPECT_TRUE(validateOp(kJailRemove, "3"));
    EXPECT_TRUE(validateOp(kJailRemove, "123456789"));
    EXPECT_FALSE(validateOp(kJailRemove, "0"));
    EXPECT_FALSE(validateOp(kJailRemove, "07"));
    EXPECT_FALSE(validateOp(kJailRemove, "-a"));               // jail -r -a would remove all
    EXPECT_FALSE(validateOp(kJailRemove, "web"));
    EXPECT_FALSE(validateOp(kJailRemove, "1234567890"));
}

TEST(PrivOps, FirewallBlockInIsAOneWayTrigger) {
    EXPECT_TRUE(knownVerb(kFirewallBlockIn));
    EXPECT_TRUE(validateOp(kFirewallBlockIn, "1"));
    EXPECT_FALSE(validateOp(kFirewallBlockIn, "0"));            // undo is firewall.enable
    EXPECT_FALSE(validateOp(kFirewallBlockIn, ""));
    EXPECT_FALSE(validateOp(kFirewallBlockIn, "pass in all"));  // never a client ruleset
}

TEST(PrivOps, WlanStateOnlyWifiInterfaces) {
    EXPECT_TRUE(knownVerb(kWlanState));
    EXPECT_TRUE(validateOp(kWlanState, "wlan0:0"));
    EXPECT_TRUE(validateOp(kWlanState, "wlan12:1"));
    EXPECT_TRUE(validateOp(kWlanState, "wlan999:1"));
    EXPECT_FALSE(validateOp(kWlanState, "em0:0"));               // wired: never
    EXPECT_FALSE(validateOp(kWlanState, "lo0:0"));
    EXPECT_FALSE(validateOp(kWlanState, "wlan:0"));
    EXPECT_FALSE(validateOp(kWlanState, "wlan0:2"));
    EXPECT_FALSE(validateOp(kWlanState, "wlan0"));
    EXPECT_FALSE(validateOp(kWlanState, "wlan0:0 destroy"));
    EXPECT_FALSE(validateOp(kWlanState, "wlan1000:0"));
    EXPECT_FALSE(validateOp(kWlanState, "wlanx:1"));
}
