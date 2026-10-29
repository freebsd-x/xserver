#pragma once

#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// Pure parsers for the FreeBSD system-admin services (com.freebsdx.Services,
// Jails, ZfsPools, BootEnv, NTP). Each service runs a base-system tool via
// posix_spawn (or reads an rc script) and feeds the text here. Dependency-free /
// unit-testable; no syscalls.
namespace fbx::xserver::sysadmin {

namespace detail {
inline std::vector<std::string_view> lines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t nl = s.find('\n', start);
        if (nl == std::string_view::npos) nl = s.size();
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}
inline std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(sep, start);
        out.push_back(s.substr(start, (p == std::string_view::npos ? s.size() : p) - start));
        if (p == std::string_view::npos) break;
        start = p + 1;
    }
    return out;
}
inline std::vector<std::string_view> tokens(std::string_view s) {
    std::vector<std::string_view> t;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        size_t b = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        if (i > b) t.push_back(s.substr(b, i - b));
    }
    return t;
}
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
inline uint64_t toU64(std::string_view s) {
    uint64_t v = 0;
    for (char c : s) { if (c < '0' || c > '9') break; v = v * 10 + static_cast<uint64_t>(c - '0'); }
    return v;
}
}  // namespace detail

// ── rc(8) scripts ────────────────────────────────────────────────────────────

// What we need from one rc script to show it as a daemon.
struct RcDaemon {
    std::string name;      // rc script name, e.g. "sshd"
    std::string procname;  // process comm to look for, e.g. "sshd", "dbus-daemon"
};

// Value of the first top-level `key=` assignment (quotes stripped), or "".
inline std::string rcVar(std::string_view script, std::string_view key) {
    for (auto line : detail::lines(script)) {
        line = detail::trim(line);
        if (line.substr(0, key.size()) != key || line.size() <= key.size() || line[key.size()] != '=')
            continue;
        std::string_view v = line.substr(key.size() + 1);
        size_t hash = v.find(" #");                       // trailing comment
        if (hash != std::string_view::npos) v = v.substr(0, hash);
        v = detail::trim(v);
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
            v = v.substr(1, v.size() - 2);
        return std::string(v);
    }
    return {};
}

// Parse an rc script into an RcDaemon. Only scripts with a pidfile= are
// long-running daemons; one-shot scripts (hostid, newsyslog, …) return false.
// The process name comes from procname= when set (scripts run under daemon(8)
// set command=/usr/sbin/daemon and procname=<real binary>), else command=;
// ${name} is expanded and the basename taken.
inline bool parseRcScript(std::string_view script_name, std::string_view script, RcDaemon& out) {
    if (rcVar(script, "pidfile").empty()) return false;
    std::string name = rcVar(script, "name");
    if (name.empty()) name = std::string(script_name);
    std::string prog = rcVar(script, "procname");
    if (prog.empty()) prog = rcVar(script, "command");
    if (prog.empty()) return false;
    for (std::string_view var : {"${name}", "$name"}) {
        size_t p;
        while ((p = prog.find(var)) != std::string::npos) prog.replace(p, var.size(), name);
    }
    if (prog.find('$') != std::string::npos) return false;   // unexpandable
    size_t slash = prog.rfind('/');
    out.name     = std::string(script_name);
    out.procname = (slash == std::string::npos) ? prog : prog.substr(slash + 1);
    return !out.procname.empty();
}

// Process comm names are truncated by the kernel (COMMLEN = 19); compare that way.
inline bool commMatches(std::string_view comm, std::string_view procname) {
    constexpr size_t kCommLen = 19;
    return comm.substr(0, kCommLen) == procname.substr(0, kCommLen);
}

// ── zpool(8) ─────────────────────────────────────────────────────────────────

struct Pool {
    std::string name;
    std::string health;
    uint64_t    size_bytes  = 0;
    uint64_t    alloc_bytes = 0;
};

// `zpool list -Hp -o name,size,alloc,health` — tab-separated, exact bytes.
inline std::vector<Pool> parseZpoolList(std::string_view out) {
    std::vector<Pool> pools;
    for (auto line : detail::lines(out)) {
        auto f = detail::split(line, '\t');
        if (f.size() < 4 || f[0].empty()) continue;
        Pool p;
        p.name        = std::string(f[0]);
        p.size_bytes  = detail::toU64(f[1]);
        p.alloc_bytes = detail::toU64(f[2]);
        p.health      = std::string(detail::trim(f[3]));
        pools.push_back(std::move(p));
    }
    return pools;
}

// From `zpool status <pool>`: is a scrub running, and how far (0-100)?
//   scan: scrub in progress since Sun Jul 25 10:00:00 2026
//         1.2G / 10G scanned at 100M/s, 456M / 10G issued, 45.67% done, …
inline bool parseScrubProgress(std::string_view status, uint32_t& pct) {
    pct = 0;
    if (status.find("scrub in progress") == std::string_view::npos) return false;
    size_t done = status.find("% done");
    if (done != std::string_view::npos) {
        size_t b = done;
        while (b > 0 && ((status[b - 1] >= '0' && status[b - 1] <= '9') || status[b - 1] == '.')) --b;
        pct = static_cast<uint32_t>(std::atof(std::string(status.substr(b, done - b)).c_str()));
        if (pct > 100) pct = 100;
    }
    return true;
}

// ── bectl(8) ─────────────────────────────────────────────────────────────────

struct BootEnv {
    std::string name;
    std::string created;     // "2026-07-20 09:15"
    bool        active    = false;   // booted now ("N")
    bool        next_boot = false;   // activated for next boot ("R")
};

// `bectl list -H` — tab-separated: name, active flags (N/R/NR/-), mountpoint,
// space, created.
inline std::vector<BootEnv> parseBectlList(std::string_view out) {
    std::vector<BootEnv> bes;
    for (auto line : detail::lines(out)) {
        auto f = detail::split(line, '\t');
        if (f.size() < 5 || f[0].empty()) continue;
        BootEnv b;
        b.name      = std::string(f[0]);
        b.active    = f[1].find('N') != std::string_view::npos;
        b.next_boot = f[1].find('R') != std::string_view::npos;
        b.created   = std::string(detail::trim(f[4]));
        bes.push_back(std::move(b));
    }
    return bes;
}

// ── jls(8) ───────────────────────────────────────────────────────────────────

struct Jail {
    uint32_t    jid = 0;
    std::string hostname;
    std::string ip;          // first IPv4 address, or ""
    bool        dying = false;
};

// `jls -n jid host.hostname ip4.addr dying` — one jail per line of key=value
// pairs. ip4.addr may be a comma list; keep the first.
inline std::vector<Jail> parseJls(std::string_view out) {
    std::vector<Jail> jails;
    for (auto line : detail::lines(out)) {
        Jail j;
        bool have_jid = false;
        for (auto tok : detail::tokens(line)) {
            size_t eq = tok.find('=');
            if (eq == std::string_view::npos) continue;
            std::string_view k = tok.substr(0, eq), v = tok.substr(eq + 1);
            if (k == "jid")                { j.jid = static_cast<uint32_t>(detail::toU64(v)); have_jid = !v.empty(); }
            else if (k == "host.hostname")   j.hostname = std::string(v);
            else if (k == "ip4.addr")        j.ip = std::string(v.substr(0, v.find(',')));
            else if (k == "dying")           j.dying = (v == "true" || v == "1");
        }
        if (have_jid) jails.push_back(std::move(j));
    }
    return jails;
}

// ── ntpq(8) ──────────────────────────────────────────────────────────────────

struct NtpPeerInfo {
    bool        synced = false;     // a peer is selected as system peer ('*')
    std::string server;             // the system peer, else the first listed peer
    double      offset_ms = 0.0;    // system peer's offset
    uint32_t    when_sec  = 0;      // seconds since the system peer last answered
};

// `ntpq -pn`. Data rows start with a tally character (' ', '*', '+', '-', …)
// followed by: remote refid st t when poll reach delay offset jitter.
inline NtpPeerInfo parseNtpqPeers(std::string_view out) {
    NtpPeerInfo info;
    for (auto line : detail::lines(out)) {
        if (line.empty() || line.find("remote") != std::string_view::npos || line[0] == '=') continue;
        char tally = line[0];
        auto t = detail::tokens(line.substr(1));
        if (t.size() < 10) continue;
        if (info.server.empty()) info.server = std::string(t[0]);
        if (tally == '*') {
            info.synced    = true;
            info.server    = std::string(t[0]);
            // "when": seconds, or with an m/h/d suffix once it grows; "-" → 0.
            uint64_t when = detail::toU64(t[4]);
            switch (t[4].empty() ? '\0' : t[4].back()) {
            case 'm': when *= 60;    break;
            case 'h': when *= 3600;  break;
            case 'd': when *= 86400; break;
            default: break;
            }
            info.when_sec  = static_cast<uint32_t>(when);
            info.offset_ms = std::atof(std::string(t[8]).c_str());
            break;
        }
    }
    return info;
}

// ── sshd ─────────────────────────────────────────────────────────────────────

struct SshdConfig {
    uint32_t    port = 22;
    std::string listen = "0.0.0.0";   // first ListenAddress, port suffix dropped
};

// /etc/ssh/sshd_config: first Port / ListenAddress wins (sshd's own rule).
// Keywords are case-insensitive; a Match block ends the global section.
inline SshdConfig parseSshdConfig(std::string_view text) {
    SshdConfig c;
    bool have_port = false, have_listen = false;
    auto lower = [](std::string_view s) {
        std::string o(s);
        for (char& ch : o) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        return o;
    };
    for (auto line : detail::lines(text)) {
        auto t = detail::tokens(line);
        if (t.empty() || t[0][0] == '#') continue;
        std::string kw = lower(t[0]);
        if (kw == "match") break;
        if (t.size() < 2) continue;
        if (kw == "port" && !have_port) {
            uint64_t p = detail::toU64(t[1]);
            if (p > 0 && p < 65536) { c.port = static_cast<uint32_t>(p); have_port = true; }
        } else if (kw == "listenaddress" && !have_listen) {
            std::string_view a = t[1];
            if (!a.empty() && a.front() == '[') a = a.substr(1, a.find(']') - 1);  // [::1]:22
            else if (std::count(a.begin(), a.end(), ':') == 1) a = a.substr(0, a.find(':'));
            c.listen = std::string(a);
            have_listen = true;
        }
    }
    return c;
}

// `netstat -an -p tcp`: ESTABLISHED connections whose local port is `port`
// (inbound SSH sessions). Local address is "a.b.c.d.port" / "v6addr.port".
inline uint32_t countEstablished(std::string_view out, uint32_t port) {
    uint32_t n = 0;
    std::string suffix = "." + std::to_string(port);
    for (auto line : detail::lines(out)) {
        auto t = detail::tokens(line);
        if (t.size() < 6 || t[0].substr(0, 3) != "tcp" || t[5] != "ESTABLISHED") continue;
        std::string_view local = t[3];
        if (local.size() > suffix.size() && local.substr(local.size() - suffix.size()) == suffix) ++n;
    }
    return n;
}

// ── pkg(8) ───────────────────────────────────────────────────────────────────

struct PkgUpdates {
    uint32_t    count = 0;
    std::string first;   // "firefox 132.0" — the first outdated package + its new version
};

// `pkg version -vRl '<' -U`: "firefox-131.0   <   needs updating (remote has 132.0)".
inline PkgUpdates parsePkgOutdated(std::string_view out) {
    PkgUpdates u;
    for (auto line : detail::lines(out)) {
        auto t = detail::tokens(line);
        if (t.size() < 2 || t[1] != "<") continue;
        ++u.count;
        if (!u.first.empty()) continue;
        std::string_view pkg = t[0];
        size_t dash = pkg.rfind('-');
        std::string name(dash == std::string_view::npos ? pkg : pkg.substr(0, dash));
        constexpr std::string_view kHas = "remote has ";
        size_t h = line.find(kHas);
        if (h != std::string_view::npos) {
            std::string_view ver = line.substr(h + kHas.size());
            ver = ver.substr(0, ver.find(')'));
            u.first = name + " " + std::string(ver);
        } else {
            u.first = name;
        }
    }
    return u;
}

// ── Wi-Fi interfaces ─────────────────────────────────────────────────────────

// `ifconfig -l [-u]` → the wlanN interfaces (what airplane mode switches).
inline std::vector<std::string> parseWlanIfaces(std::string_view out) {
    std::vector<std::string> v;
    for (auto t : detail::tokens(detail::trim(out.substr(0, out.find('\n'))))) {
        if (t.size() < 5 || t.size() > 7 || t.substr(0, 4) != "wlan") continue;
        bool digits = true;
        for (char c : t.substr(4)) digits = digits && c >= '0' && c <= '9';
        if (digits) v.emplace_back(t);
    }
    return v;
}

}  // namespace fbx::xserver::sysadmin
