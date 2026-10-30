#pragma once

#include "xserver/dns_resolver.h"
#include <string_view>

// The privilege broker's allowlist + argument validator — the security boundary
// between the unprivileged compositor and the root xsessiond daemon. A broker
// differs from a setuid-anything helper precisely in this: it performs a FIXED
// set of named verbs with VALIDATED arguments, never an arbitrary command. Both
// the client and the daemon validate through here; the daemon NEVER acts on a
// verb/arg pair that fails validateOp(). Pure / dependency-free / unit-testable.
namespace fbx::xserver::privops {

// Allowlisted verbs. Anything not here is rejected.
inline constexpr const char* kResolvConfWrite  = "resolvconf.write";   // arg = resolv.conf body
inline constexpr const char* kAudioDefaultUnit = "audio.default_unit"; // arg = pcm unit number
inline constexpr const char* kFirewallEnable   = "firewall.enable";    // arg = "1" (enable) / "0" (disable)
inline constexpr const char* kKmodUnload       = "kmod.unload";        // arg = module name
inline constexpr const char* kServiceStart     = "service.start";      // arg = rc script name
inline constexpr const char* kServiceStop      = "service.stop";       // arg = rc script name
inline constexpr const char* kZfsScrub         = "zfs.scrub";          // arg = pool name
inline constexpr const char* kBeActivate       = "be.activate";        // arg = boot environment name
inline constexpr const char* kBeDestroy        = "be.destroy";         // arg = boot environment name
inline constexpr const char* kJailRemove       = "jail.remove";        // arg = decimal jid
inline constexpr const char* kFirewallBlockIn  = "firewall.blockin";   // arg = "1": load the block-all-inbound ruleset
inline constexpr const char* kWlanState        = "net.wlan";           // arg = "wlanN:0" (down) / "wlanN:1" (up)
inline constexpr const char* kAdBlockApply     = "adblock.apply";      // arg = "1" (apply the user's list) / "0" (remove)

inline bool knownVerb(std::string_view v) noexcept {
    return v == kResolvConfWrite || v == kAudioDefaultUnit ||
           v == kFirewallEnable  || v == kKmodUnload ||
           v == kServiceStart    || v == kServiceStop ||
           v == kZfsScrub        || v == kBeActivate || v == kBeDestroy ||
           v == kJailRemove      || v == kFirewallBlockIn || v == kWlanState ||
           v == kAdBlockApply;
}

// A name passed to a root command as its own argv element: 1..max chars from
// [A-Za-z0-9_-] plus `extra`, starting with an alphanumeric so it can never be
// read as a flag. No '/', whitespace or shell metacharacters.
inline bool safeName(std::string_view arg, size_t max, std::string_view extra) noexcept {
    if (arg.empty() || arg.size() > max) return false;
    auto alnum = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    if (!alnum(arg.front())) return false;
    for (char c : arg)
        if (!alnum(c) && c != '_' && c != '-' && extra.find(c) == std::string_view::npos)
            return false;
    return true;
}

// rc services the desktop session itself depends on. Stopping (or restarting)
// them from a Settings panel would kill the session, so the broker refuses.
inline bool sessionCriticalService(std::string_view name) noexcept {
    return name == "xsessiond" || name == "seatd";
}

// True only when `verb` is known AND `arg` is safe for it. This is the gate the
// daemon honours before doing anything as root.
inline bool validateOp(std::string_view verb, std::string_view arg) noexcept {
    if (verb == kResolvConfWrite) {
        // The entire resolv.conf body. Every line must be blank, a '#' comment,
        // or "nameserver <field>" with an injection-safe field — nothing else,
        // so a caller can't smuggle in arbitrary resolver directives or shell.
        if (arg.empty() || arg.size() > 4096) return false;
        size_t pos = 0;
        while (pos < arg.size()) {
            size_t nl = arg.find('\n', pos);
            std::string_view line = arg.substr(pos, (nl == std::string_view::npos ? arg.size() : nl) - pos);
            pos = (nl == std::string_view::npos) ? arg.size() : nl + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty() || line[0] == '#') continue;              // blank / comment
            constexpr std::string_view kNs = "nameserver ";
            if (line.substr(0, kNs.size()) != kNs) return false;       // only nameserver lines
            std::string_view ns = line.substr(kNs.size());
            if (ns.empty() || !dnsresolver::validField(ns)) return false;
        }
        return true;
    }
    if (verb == kAudioDefaultUnit) {
        // A small decimal pcm unit (1–3 digits), nothing else.
        if (arg.empty() || arg.size() > 3) return false;
        for (char c : arg) if (c < '0' || c > '9') return false;
        return true;
    }
    if (verb == kFirewallEnable) {
        // Exactly "0" or "1" — the daemon maps this to a FIXED pfctl argv
        // (-d / -e); the arg never becomes part of the command line.
        return arg == "0" || arg == "1";
    }
    if (verb == kKmodUnload) {
        // A kld module name: [A-Za-z0-9._-], ≤64, and must start with an
        // alphanumeric so it can never be read as a flag (e.g. "-f"). The daemon
        // passes it as a distinct argv element (no shell), so with these chars
        // there is no injection surface.
        if (arg.empty() || arg.size() > 64) return false;
        char c0 = arg.front();
        bool alnum0 = (c0 >= '0' && c0 <= '9') || (c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z');
        if (!alnum0) return false;
        for (char c : arg) {
            bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                      (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-';
            if (!ok) return false;
        }
        return true;
    }
    if (verb == kServiceStart || verb == kServiceStop) {
        // An rc(8) script name; the daemon runs FIXED `service <name> start|stop`.
        return safeName(arg, 32, "") && !sessionCriticalService(arg);
    }
    if (verb == kZfsScrub || verb == kBeActivate || verb == kBeDestroy) {
        // ZFS pool / boot environment names: also '.' and ':' (e.g.
        // "15.1-RELEASE-p3_2026-10-26_213713"). Never '/', so a BE argument
        // can't name a dataset path.
        return safeName(arg, 64, ".:");
    }
    if (verb == kJailRemove) {
        // A positive decimal jid (≤9 digits).
        if (arg.empty() || arg.size() > 9 || arg == "0" || arg.front() == '0') return false;
        for (char c : arg) if (c < '0' || c > '9') return false;
        return true;
    }
    if (verb == kFirewallBlockIn) {
        // One-way trigger; the daemon loads a FIXED compiled-in ruleset. The
        // way back is firewall.enable "1" (reloads /etc/pf.conf).
        return arg == "1";
    }
    if (verb == kWlanState) {
        // "wlan<1-3 digits>:<0|1>" — Wi-Fi interfaces only, never wired ones.
        if (arg.size() < 7 || arg.size() > 9 || arg.substr(0, 4) != "wlan") return false;
        size_t colon = arg.find(':');
        if (colon == std::string_view::npos || colon < 5 || colon > 7 || colon + 2 != arg.size()) return false;
        for (char c : arg.substr(4, colon - 4)) if (c < '0' || c > '9') return false;
        return arg.back() == '0' || arg.back() == '1';
    }
    if (verb == kAdBlockApply) {
        // Exactly "0"/"1". The domains never travel in the arg: the daemon reads
        // the caller's own list file (see xsessiond) and re-validates every name.
        return arg == "0" || arg == "1";
    }
    return false;   // unknown verb
}

}  // namespace fbx::xserver::privops
