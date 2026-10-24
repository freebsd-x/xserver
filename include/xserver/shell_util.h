#pragma once
// shell_util.h — centralised shell-safety helpers for XBus services.
//
// Use shellQuote() to safely interpolate arbitrary user strings into
// popen()/system() commands.  Use the isValidXxx() predicates as a
// fast whitelist rejection guard before building commands from
// structured identifiers (MAC addresses, interface names, ZFS names).

#include <cctype>
#include <string>
#include <string_view>

namespace fbx::xserver {

// shellQuote ──────────────────────────────────────────────────────────────────
// Wraps sv in POSIX single quotes, escaping any embedded single quote as '\''.
// The result is safe to embed in a shell command string regardless of what
// characters sv contains (including spaces, $, backticks, semicolons, etc.).
//
// Example:  shellQuote("it's alive")  →  'it'\''s alive'
inline std::string shellQuote(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() + 2);
    out += '\'';
    for (char c : sv) {
        if (c == '\'') out += "'\\''"; // close quote, escaped quote, reopen
        else           out += c;
    }
    out += '\'';
    return out;
}

// isValidMacAddress ───────────────────────────────────────────────────────────
// Accepts exactly the canonical form "XX:XX:XX:XX:XX:XX" (17 characters,
// hex digits separated by colons).  Rejects everything else.
inline bool isValidMacAddress(std::string_view sv) noexcept {
    if (sv.size() != 17) return false;
    for (size_t i = 0; i < 17; ++i) {
        if (i % 3 == 2) {
            if (sv[i] != ':') return false;
        } else {
            if (!std::isxdigit(static_cast<unsigned char>(sv[i]))) return false;
        }
    }
    return true;
}

// isValidInterfaceName ────────────────────────────────────────────────────────
// Accepts network/WireGuard interface names: alphanumeric + hyphen + underscore,
// 1–16 characters.  Rejects any shell-special character.
inline bool isValidInterfaceName(std::string_view sv) noexcept {
    if (sv.empty() || sv.size() > 16) return false;
    for (unsigned char c : sv) {
        if (!std::isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

// isValidZfsName ──────────────────────────────────────────────────────────────
// Accepts ZFS dataset/snapshot component names: alphanumeric plus the
// characters ZFS itself allows (underscore, hyphen, colon, period, slash).
// Rejects shell-special characters ($, `, ;, &, |, etc.).
inline bool isValidZfsName(std::string_view sv) noexcept {
    if (sv.empty() || sv.size() > 256) return false;
    for (unsigned char c : sv) {
        if (!std::isalnum(c) &&
            c != '_' && c != '-' && c != ':' && c != '.' && c != '/')
            return false;
    }
    return true;
}

// isValidPrinterName ──────────────────────────────────────────────────────────
// CUPS printer names: alphanumeric + hyphen + underscore + dot, 1–64 chars.
inline bool isValidPrinterName(std::string_view sv) noexcept {
    if (sv.empty() || sv.size() > 64) return false;
    for (unsigned char c : sv) {
        if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

} // namespace fbx::xserver
