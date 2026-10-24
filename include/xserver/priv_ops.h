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

inline bool knownVerb(std::string_view v) noexcept {
    return v == kResolvConfWrite || v == kAudioDefaultUnit ||
           v == kFirewallEnable  || v == kKmodUnload;
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
    return false;   // unknown verb
}

}  // namespace fbx::xserver::privops
