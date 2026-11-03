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

TEST(PrivOps, AdBlockApplyIsOnlyOnOrOff) {
    EXPECT_TRUE(knownVerb(kAdBlockApply));
    EXPECT_TRUE(validateOp(kAdBlockApply, "1"));
    EXPECT_TRUE(validateOp(kAdBlockApply, "0"));
    EXPECT_FALSE(validateOp(kAdBlockApply, ""));
    EXPECT_FALSE(validateOp(kAdBlockApply, "/etc/master.passwd"));   // never a path
    EXPECT_FALSE(validateOp(kAdBlockApply, "0.0.0.0 ads.example.com"));
}

// ── What the broker refuses to touch as root ─────────────────────────────────

TEST(PrivOps, KmodUnloadRefusesProtectedModules) {
    for (const char* m : {"mac_portacl", "mac_bsdextended", "mac_portacl.ko",          // security policy
                          "pf", "pflog", "ipfw", "ipfilter", "ipfw.ko",                 // packet filters
                          "drm", "drm2", "i915kms", "amdgpu", "linuxkpi", "nvidia-modeset",   // GPU
                          "evdev", "hidbus", "hkbd", "hms", "ukbd", "psm", "kbdmux"})        // input
        EXPECT_FALSE(validateOp(kKmodUnload, m)) << m;
    // Ordinary modules still can be unloaded.
    EXPECT_TRUE(validateOp(kKmodUnload, "ichsmb"));
    EXPECT_TRUE(validateOp(kKmodUnload, "if_wg"));
    EXPECT_TRUE(validateOp(kKmodUnload, "pflash"));   // a "pf" prefix alone isn't a filter
}

TEST(PrivOps, ServiceStopRefusesSecurityServices) {
    for (const char* svc : {"xsessiond", "seatd", "auditd", "blacklistd"}) {
        EXPECT_FALSE(validateOp(kServiceStop, svc)) << svc;
        EXPECT_FALSE(validateOp(kServiceStart, svc)) << svc;
    }
    EXPECT_TRUE(validateOp(kServiceStop, "sshd"));
}

// Property: whatever validateOp accepts can never be read as an option, name a
// path, or carry a shell metacharacter or control character — for every verb,
// over many random arguments. (resolv.conf's body is the one multi-line arg;
// its lines are checked as nameserver lines instead.)
TEST(PrivOps, AcceptedArgumentsAreAlwaysInert) {
    const char* verbs[] = {kAudioDefaultUnit, kFirewallEnable, kKmodUnload, kServiceStart, kServiceStop,
                           kZfsScrub, kBeActivate, kBeDestroy, kJailRemove, kFirewallBlockIn,
                           kWlanState, kAdBlockApply, kResolvConfWrite};
    const std::string alphabet = "abcwlanpfmac_0123456789.:-/ \t\n;|&$`'\"\\*?<>()";
    uint32_t seed = 20261103;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    int accepted = 0;
    for (const char* verb : verbs) {
        for (int i = 0; i < 20000; ++i) {
            std::string arg;
            size_t len = rnd() % 12;
            if (std::string_view(verb) == kResolvConfWrite) arg = "nameserver ";
            for (size_t k = 0; k < len; ++k) arg += alphabet[rnd() % alphabet.size()];
            if (!validateOp(verb, arg)) continue;
            ++accepted;
            if (std::string_view(verb) == kResolvConfWrite) {
                EXPECT_EQ(arg.find_first_of(";|&$`'\"\\*?<>()"), std::string::npos) << arg;
                continue;
            }
            EXPECT_NE(arg.front(), '-') << verb << ": " << arg;
            EXPECT_EQ(arg.find('/'), std::string::npos) << verb << ": " << arg;
            EXPECT_EQ(arg.find_first_of(" \t\n;|&$`'\"\\*?<>()"), std::string::npos) << verb << ": " << arg;
        }
    }
    EXPECT_GT(accepted, 100);   // the generator does reach the accepting paths
}
