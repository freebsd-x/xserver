#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Pure parsers/validators for com.freebsdx.NetworkShare(s), VirtualMachines and
// Containers. Services feed them `mount -p`, /etc/fstab and `podman ps`
// output. Dependency-free / unit-testable; no syscalls.
namespace fbx::xserver::infra {

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
inline std::vector<std::string_view> fields(std::string_view s) {
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
inline bool charsIn(std::string_view s, std::string_view extra) {
    for (char c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  extra.find(c) != std::string_view::npos;
        if (!ok) return false;
    }
    return true;
}
}  // namespace detail

// ── Network shares ───────────────────────────────────────────────────────────

enum Protocol : uint32_t { kSmb = 0, kNfs = 1 };

struct Share {
    uint32_t    protocol = kSmb;
    std::string host;
    std::string share;       // always with a leading '/', e.g. "/media", "/export/home"
    std::string user;        // SMB only, may be empty
    std::string mountpoint;
    bool        mounted = false;
};

// "//USER@HOST/SHARE" (smbfs) or "host:/path" (nfs) → Share. False otherwise.
inline bool parseSpec(std::string_view spec, std::string_view fstype, Share& s) {
    if (fstype == "smbfs") {
        if (spec.substr(0, 2) != "//") return false;
        spec.remove_prefix(2);
        size_t slash = spec.find('/');
        if (slash == std::string_view::npos || slash == 0) return false;
        std::string_view who = spec.substr(0, slash);
        size_t at = who.find('@');
        s.protocol = kSmb;
        s.user = at == std::string_view::npos ? "" : std::string(who.substr(0, at));
        s.host = std::string(at == std::string_view::npos ? who : who.substr(at + 1));
        s.share = std::string(spec.substr(slash));
        return !s.host.empty() && s.share.size() > 1;
    }
    if (fstype == "nfs") {
        size_t colon = spec.find(':');
        if (colon == std::string_view::npos || colon == 0) return false;
        s.protocol = kNfs;
        s.host = std::string(spec.substr(0, colon));
        s.share = std::string(spec.substr(colon + 1));
        return !s.share.empty() && s.share[0] == '/';
    }
    return false;
}

// Network shares: everything mounted now (`mount -p -t nfs,smbfs`) plus the
// /etc/fstab entries that aren't, keyed by mount point.
inline std::vector<Share> collectShares(std::string_view mount_p, std::string_view fstab) {
    std::vector<Share> out;
    auto scan = [&out](std::string_view text, bool mounted) {
        for (auto line : detail::lines(text)) {
            auto f = detail::fields(line);
            if (f.size() < 3 || f[0][0] == '#') continue;
            Share s;
            if (!parseSpec(f[0], f[2], s)) continue;
            s.mountpoint = std::string(f[1]);
            s.mounted = mounted;
            bool dup = false;
            for (const auto& o : out) dup = dup || o.mountpoint == s.mountpoint;
            if (!dup) out.push_back(std::move(s));
        }
    };
    scan(mount_p, true);
    scan(fstab, false);
    return out;
}

// Stable id from the mount point (FNV-1a); never 0.
inline uint32_t shareId(std::string_view mountpoint) {
    uint32_t h = 2166136261u;
    for (char c : mountpoint) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
    return h ? h : 1u;
}

// Wire-supplied mount arguments go straight into an argv (no shell), but must
// still not look like options or smuggle path tricks.
inline bool validHost(std::string_view h) {
    return !h.empty() && h.size() <= 63 && h[0] != '-' && h[0] != '.' && detail::charsIn(h, ".-:");
}
inline bool validSharePath(std::string_view p) {
    if (p.size() < 2 || p.size() > 63 || p[0] != '/') return false;
    if (p.find("/../") != std::string_view::npos || p.ends_with("/..")) return false;
    return detail::charsIn(p, "._-/");
}
inline bool validUser(std::string_view u) {
    return u.size() <= 31 && (u.empty() || u[0] != '-') && detail::charsIn(u, "._-");
}

// ── Containers (podman) ──────────────────────────────────────────────────────

struct Container {
    std::string id, name, image;
    uint32_t    state = 0;   // 0=stopped, 1=running, 2=paused, 3=error
};

// `podman ps -a --format '{{.ID}}\t{{.Names}}\t{{.Image}}\t{{.State}}'`.
inline std::vector<Container> parsePodmanPs(std::string_view out) {
    std::vector<Container> cs;
    for (auto line : detail::lines(out)) {
        std::vector<std::string_view> f;
        size_t start = 0;
        while (true) {
            size_t tab = line.find('\t', start);
            f.push_back(line.substr(start, (tab == std::string_view::npos ? line.size() : tab) - start));
            if (tab == std::string_view::npos) break;
            start = tab + 1;
        }
        if (f.size() < 4 || f[0].empty()) continue;
        Container c;
        c.id    = std::string(f[0].substr(0, 12));
        c.name  = std::string(f[1]);
        c.image = std::string(f[2]);
        std::string_view st = f[3];
        c.state = st == "running" ? 1u
                : st == "paused" ? 2u
                : (st == "exited" || st == "created" || st == "stopped" || st == "configured") ? 0u
                : 3u;
        cs.push_back(std::move(c));
    }
    return cs;
}

// A container id from the wire: hex, 12-64 chars.
inline bool validContainerId(std::string_view id) {
    if (id.size() < 12 || id.size() > 64) return false;
    for (char c : id)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// ── bhyve ────────────────────────────────────────────────────────────────────

// A running bhyve VM appears as /dev/vmm/<name>; names are what bhyve accepts.
inline bool validVmName(std::string_view n) {
    return !n.empty() && n.size() <= 31 && n[0] != '-' && n[0] != '.' && detail::charsIn(n, "._-");
}

}  // namespace fbx::xserver::infra
