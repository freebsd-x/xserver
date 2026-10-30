#pragma once

#include <algorithm>
#include <cstddef>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

// Pure helpers for com.freebsdx.AdBlock: a DNS sinkhole kept as a marked block
// of "0.0.0.0 <domain>" lines in /etc/hosts. The session fetches a hosts-format
// blocklist as the user and stores it normalized (one domain per line); the
// root broker (xsessiond, verb adblock.apply) re-parses that file with the same
// strict rules and rewrites only its own block. Every line it writes sinkholes
// a validated domain name — never an address of the list's choosing — so a
// tampered list can block sites but can't redirect one.
// Dependency-free / unit-testable (shared by the compositor and xsessiond).
namespace fbx::xserver::adblock {

inline constexpr std::string_view kBlockBegin = "# BEGIN FreeBSD-X AdBlock (managed: Settings → AdBlock)";
inline constexpr std::string_view kBlockEnd   = "# END FreeBSD-X AdBlock";
inline constexpr size_t kMaxDomains   = 300000;            // hosts lookups are linear
inline constexpr size_t kMaxListBytes = 32u * 1024 * 1024;  // raw download / stored list

// The list the session downloads (StevenBlack's unified hosts, MIT licensed).
inline constexpr const char* kDefaultListUrl =
    "https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts";

// The normalized list, relative to the user's home. The root broker opens
// exactly this path (component by component, no symlinks, owned by the user).
inline constexpr const char* kListDirs[] = {".cache", "freebsd-x", "adblock"};
inline constexpr const char* kListFile   = "domains";

// "Jan 14 2026"-style date for the AdBlockStatus.list_updated_ field. Uses UTC
// so the format is deterministic (timezone-independent); the label is coarse.
inline std::string formatListDate(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[24];
    std::strftime(buf, sizeof(buf), "%b %d %Y", &tm);
    return buf;
}

// A lowercase DNS name with at least two labels: [a-z0-9-] labels of 1..63
// chars that don't start or end with '-', ≤253 chars in all, and a TLD that
// isn't all digits (so no IPv4 address passes).
inline bool validDomain(std::string_view d) {
    if (d.empty() || d.size() > 253 || d.front() == '.' || d.back() == '.') return false;
    size_t labels = 0, start = 0;
    while (start <= d.size()) {
        size_t dot = d.find('.', start);
        if (dot == std::string_view::npos) dot = d.size();
        std::string_view label = d.substr(start, dot - start);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
        bool all_digits = true;
        for (char c : label) {
            bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
            if (!ok) return false;
            if (c < '0' || c > '9') all_digits = false;
        }
        ++labels;
        if (dot == d.size()) return labels >= 2 && !all_digits;
        start = dot + 1;
    }
    return false;
}

// Names the system itself relies on are never sinkholed, whatever a list says:
// loopback names and FreeBSD's package / update / time servers.
inline bool neverBlock(std::string_view d) {
    auto is_or_under = [&](std::string_view root) {
        return d == root || (d.size() > root.size() && d.ends_with(root) &&
                             d[d.size() - root.size() - 1] == '.');
    };
    return is_or_under("localhost") || d == "localhost.localdomain" || is_or_under("local") ||
           is_or_under("freebsd.org") || is_or_under("pool.ntp.org");
}

// Domains from a hosts-format list ("0.0.0.0 ads.example.com", "127.0.0.1 x",
// or a bare "ads.example.com" per line, '#' comments). Lowercased, validated,
// de-duplicated and sorted; anything else is skipped. At most kMaxDomains.
inline std::vector<std::string> parseList(std::string_view text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size() && out.size() < kMaxDomains) {
        size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, (nl == std::string_view::npos ? text.size() : nl) - pos);
        pos = nl == std::string_view::npos ? text.size() : nl + 1;
        if (auto h = line.find('#'); h != std::string_view::npos) line = line.substr(0, h);
        // Up to two whitespace-separated fields: [address] domain.
        std::vector<std::string_view> f;
        size_t i = 0;
        while (i < line.size() && f.size() < 3) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
            size_t j = i;
            while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '\r') ++j;
            if (j > i) f.push_back(line.substr(i, j - i));
            i = j;
        }
        std::string_view name;
        if (f.size() == 1) name = f[0];
        else if (f.size() == 2 && (f[0] == "0.0.0.0" || f[0] == "127.0.0.1" || f[0] == "::" || f[0] == "::1"))
            name = f[1];
        else continue;
        std::string d(name);
        for (char& c : d) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (validDomain(d) && !neverBlock(d)) out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// The stored list: one domain per line.
inline std::string formatList(const std::vector<std::string>& domains) {
    std::string s;
    for (const auto& d : domains) { s += d; s += '\n'; }
    return s;
}

// /etc/hosts with our block replaced by `domains` (or removed when empty).
// Everything outside the markers is kept byte for byte; a BEGIN marker with no
// END after it (a hand edit) isn't treated as our block, so nothing below it
// is lost.
inline std::string spliceHosts(std::string_view hosts, const std::vector<std::string>& domains) {
    std::string out, block;   // block: lines since BEGIN, until END confirms them
    bool inside = false;
    size_t pos = 0;
    while (pos < hosts.size()) {
        size_t nl = hosts.find('\n', pos);
        size_t end = nl == std::string_view::npos ? hosts.size() : nl + 1;
        std::string_view line = hosts.substr(pos, end - pos);
        pos = end;
        std::string_view bare = line;
        while (!bare.empty() && (bare.back() == '\n' || bare.back() == '\r')) bare.remove_suffix(1);
        if (!inside && bare == kBlockBegin) { inside = true; block = line; continue; }
        if (inside) {
            if (bare == kBlockEnd) { inside = false; block.clear(); }
            else block += line;
            continue;
        }
        out += line;
    }
    if (inside) out += block;   // unterminated: keep it as it was
    if (domains.empty()) return out;
    if (!out.empty() && out.back() != '\n') out += '\n';
    out += kBlockBegin; out += '\n';
    for (const auto& d : domains) { out += "0.0.0.0 "; out += d; out += '\n'; }
    out += kBlockEnd; out += '\n';
    return out;
}

// How many domains our block in /etc/hosts sinkholes (0 = AdBlock is off).
inline size_t blockedCount(std::string_view hosts) {
    size_t n = 0, pos = 0;
    bool inside = false;
    while (pos < hosts.size()) {
        size_t nl = hosts.find('\n', pos);
        std::string_view line = hosts.substr(pos, (nl == std::string_view::npos ? hosts.size() : nl) - pos);
        pos = nl == std::string_view::npos ? hosts.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line == kBlockBegin) inside = true;
        else if (line == kBlockEnd) inside = false;
        else if (inside && line.starts_with("0.0.0.0 ")) ++n;
    }
    return n;
}

}  // namespace fbx::xserver::adblock
