#pragma once

#include <string>
#include <string_view>

// Pure helpers for com.freebsdx.DnsConfig (resolver settings). Dependency-free /
// unit-testable. (firstNameserver for reading current resolv.conf lives in
// net_config.h / netcfg.)
namespace fbx::xserver::dnsresolver {

// A resolver field (IPv4/IPv6/hostname) is valid when empty or composed only of
// [0-9 a-z A-Z . : -] — rejects whitespace/newlines so a value can't inject
// extra resolv.conf directives.
inline bool validField(std::string_view s) noexcept {
    for (char c : s) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') || c == '.' || c == ':' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// resolv.conf body for an explicit primary/secondary pair ("" when no primary).
inline std::string resolvConfBody(std::string_view primary, std::string_view secondary) {
    std::string s;
    if (!primary.empty())   s += "nameserver " + std::string(primary) + "\n";
    if (!secondary.empty()) s += "nameserver " + std::string(secondary) + "\n";
    if (!s.empty()) s = "# written by com.freebsdx.DnsConfig\n" + s;
    return s;
}

}  // namespace fbx::xserver::dnsresolver
