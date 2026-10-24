#pragma once

#include "shared/protocol.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

// Pure helpers for the DNS + Proxy config services. Parsing/formatting is kept
// dependency-free and inline so it can be unit-tested without touching the
// filesystem or the XBus.
namespace fbx::xserver::netcfg {

// ── DNS ─────────────────────────────────────────────────────────────────────

// First "nameserver <ip>" from resolv.conf text; "" if none.
inline std::string firstNameserver(std::string_view resolv) noexcept {
    size_t pos = 0;
    while (pos <= resolv.size()) {
        size_t nl = resolv.find('\n', pos);
        std::string_view line = resolv.substr(pos, nl == std::string_view::npos
                                                       ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? resolv.size() + 1 : nl + 1;
        // trim leading ws
        size_t b = 0; while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
        line = line.substr(b);
        if (line.rfind("nameserver", 0) != 0) continue;
        std::string_view rest = line.substr(10);
        size_t s = 0; while (s < rest.size() && (rest[s] == ' ' || rest[s] == '\t')) ++s;
        size_t e = s; while (e < rest.size() && rest[e] != ' ' && rest[e] != '\t' &&
                             rest[e] != '\r') ++e;
        if (e > s) return std::string(rest.substr(s, e - s));
    }
    return "";
}

inline constexpr uint32_t kDnsAuto = 0, kDnsDoH = 1, kDnsCustom = 2;

// Populate a DnsConfig for a mode. auto_resolver is the resolv.conf nameserver
// (used only to label Auto mode).
inline shared::DnsConfig buildDnsConfig(uint32_t mode, std::string_view custom_ip,
                                        std::string_view auto_resolver) noexcept {
    shared::DnsConfig cfg{};
    cfg.mode_ = mode;
    auto set = [](char* dst, size_t cap, std::string_view v) {
        std::strncpy(dst, std::string(v).c_str(), cap - 1);
    };
    switch (mode) {
    case kDnsDoH:
        set(cfg.resolver_, sizeof(cfg.resolver_), "Cloudflare (DoH)");
        set(cfg.doh_url_,  sizeof(cfg.doh_url_),  "https://1.1.1.1/dns-query");
        break;
    case kDnsCustom:
        set(cfg.resolver_, sizeof(cfg.resolver_),
            custom_ip.empty() ? "Custom" : custom_ip);
        set(cfg.custom_ip_, sizeof(cfg.custom_ip_), custom_ip);
        break;
    default: {  // Auto (DHCP)
        std::string label = auto_resolver.empty()
                                ? std::string("Automatic (DHCP)")
                                : std::string(auto_resolver) + " (DHCP)";
        set(cfg.resolver_, sizeof(cfg.resolver_), label);
        break;
    }
    }
    return cfg;
}

// The resolv.conf body to write for a mode (Auto leaves the file to DHCP → "").
inline std::string resolvConfFor(uint32_t mode, std::string_view custom_ip) noexcept {
    if (mode == kDnsCustom && !custom_ip.empty())
        return "# written by com.freebsdx.DNS\nnameserver " + std::string(custom_ip) + "\n";
    if (mode == kDnsDoH)
        return "# written by com.freebsdx.DNS (DoH via local proxy)\nnameserver 127.0.0.1\n";
    return "";  // Auto: don't clobber DHCP's resolv.conf
}

// ── Proxy ─────────────────────────────────────────────────────────────────────

// Serialize a ProxyConfig to a key=value blob for ~/.freebsdx/proxy.
inline std::string serializeProxy(const shared::ProxyConfig& p) noexcept {
    std::string s;
    s += "mode="       + std::to_string(p.mode_)       + "\n";
    s += "pac_url="    + std::string(p.pac_url_)       + "\n";
    s += "http_host="  + std::string(p.http_host_)     + "\n";
    s += "http_port="  + std::to_string(p.http_port_)  + "\n";
    s += "https_port=" + std::to_string(p.https_port_) + "\n";
    s += "no_proxy="   + std::string(p.no_proxy_)      + "\n";
    return s;
}

// Parse the blob back. Unknown/missing keys keep their zero defaults.
inline shared::ProxyConfig parseProxy(std::string_view text) noexcept {
    shared::ProxyConfig p{};
    size_t pos = 0;
    auto setStr = [](char* dst, size_t cap, std::string_view v) {
        std::strncpy(dst, std::string(v).c_str(), cap - 1);
    };
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos
                                                     ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
        size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r')) v.remove_suffix(1);
        if      (k == "mode")       p.mode_       = static_cast<uint32_t>(std::atoi(std::string(v).c_str()));
        else if (k == "pac_url")    setStr(p.pac_url_,   sizeof(p.pac_url_),   v);
        else if (k == "http_host")  setStr(p.http_host_, sizeof(p.http_host_), v);
        else if (k == "http_port")  p.http_port_  = static_cast<uint16_t>(std::atoi(std::string(v).c_str()));
        else if (k == "https_port") p.https_port_ = static_cast<uint16_t>(std::atoi(std::string(v).c_str()));
        else if (k == "no_proxy")   setStr(p.no_proxy_,  sizeof(p.no_proxy_),  v);
    }
    return p;
}

}  // namespace fbx::xserver::netcfg
