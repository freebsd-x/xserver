#pragma once

#include "xserver/adblock.h"

#include <cerrno>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// The root side of com.freebsdx.AdBlock (xsessiond verb adblock.apply): read
// the session user's normalized list safely and rewrite AdBlock's block of
// /etc/hosts. Paths are parameters so the logic can be tested unprivileged.
namespace fbx::xserver::adblock {

// The caller's AdBlock list, <home>/.cache/freebsd-x/adblock/domains. Opened one
// path component at a time with O_NOFOLLOW, and every directory and the file
// must belong to `uid` — so root never follows a symlink planted in the path
// and never reads a file the user couldn't have read themselves.
inline std::optional<std::string> readUserList(uid_t uid, const std::string& home) {
    namespace ab = fbx::xserver::adblock;
    auto owned = [uid](int fd, bool dir) {
        struct stat st{};
        return ::fstat(fd, &st) == 0 && st.st_uid == uid &&
               (dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode)) &&
               (dir || st.st_size <= static_cast<off_t>(ab::kMaxListBytes));
    };
    int dfd = ::open(home.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);  // the home itself may be a symlink
    if (dfd < 0 || !owned(dfd, true)) { if (dfd >= 0) ::close(dfd); return std::nullopt; }
    for (const char* comp : ab::kListDirs) {
        int next = ::openat(dfd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(dfd);
        if (next < 0) return std::nullopt;
        dfd = next;
        if (!owned(dfd, true)) { ::close(dfd); return std::nullopt; }
    }
    int fd = ::openat(dfd, ab::kListFile, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    ::close(dfd);
    if (fd < 0) return std::nullopt;
    if (!owned(fd, false)) { ::close(fd); return std::nullopt; }
    std::string body;
    char chunk[65536];
    ssize_t n;
    while ((n = ::read(fd, chunk, sizeof(chunk))) > 0 && body.size() < ab::kMaxListBytes)
        body.append(chunk, static_cast<size_t>(n));
    ::close(fd);
    if (n < 0) return std::nullopt;
    return body;
}

// adblock.apply: rewrite only the marked AdBlock block of `hosts_path` — the
// caller's list re-parsed here ("1"), or nothing ("0"). Every written line is
// "0.0.0.0 <validated name>". Replaced atomically (temp file + rename).
inline int32_t applyToHosts(bool on, uid_t uid, const std::string& home, const std::string& hosts_path) {
    namespace ab = fbx::xserver::adblock;
    std::vector<std::string> domains;
    if (on) {
        auto list = readUserList(uid, home);
        if (!list) return -ENOENT;
        domains = ab::parseList(*list);
        if (domains.empty()) return -ENODATA;
    }
    std::string hosts;
    {
        int fd = ::open(hosts_path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0 && errno != ENOENT) return -errno;
        if (fd >= 0) {
            char chunk[65536];
            ssize_t n;
            while ((n = ::read(fd, chunk, sizeof(chunk))) > 0 && hosts.size() < 2 * ab::kMaxListBytes)
                hosts.append(chunk, static_cast<size_t>(n));
            ::close(fd);
            if (n < 0) return -EIO;
        }
    }
    std::string next = ab::spliceHosts(hosts, domains);
    if (next == hosts) return 0;
    const std::string tmp = hosts_path + ".fbx-adblock";
    int wfd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (wfd < 0) return -errno;
    // Root-owned when the daemon writes it; a test runs this unprivileged.
    bool ok = (::geteuid() != 0 || ::fchown(wfd, 0, 0) == 0) && ::fchmod(wfd, 0644) == 0;
    size_t off = 0;
    while (ok && off < next.size()) {
        ssize_t w = ::write(wfd, next.data() + off, next.size() - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) ok = false; else off += static_cast<size_t>(w);
    }
    ok = ::fsync(wfd) == 0 && ok;
    ::close(wfd);
    if (!ok || ::rename(tmp.c_str(), hosts_path.c_str()) != 0) { ::unlink(tmp.c_str()); return -EIO; }
    return 0;
}

}  // namespace fbx::xserver::adblock
