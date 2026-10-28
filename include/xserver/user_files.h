#pragma once

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

// Small file helpers for services that read and write the session user's
// files (~/.config/freebsdx/…, ~/.ssh/…).
namespace fbx::xserver::userfiles {

inline std::string homeDir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (const passwd* pw = ::getpwuid(::getuid())) return pw->pw_dir;
    return "/tmp";
}

inline std::optional<std::string> readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    std::stringstream body;
    body << f.rdbuf();
    return body.str();
}

// Write via a temp file in the same directory + rename, so a crash never
// leaves a half-written file behind.
inline bool writeFileAtomic(const std::string& path, const std::string& content, mode_t mode) {
    std::string tmpl = path + ".XXXXXX";
    int fd = ::mkstemp(tmpl.data());
    if (fd < 0) return false;
    bool ok = ::fchmod(fd, mode) == 0;
    size_t off = 0;
    while (ok && off < content.size()) {
        ssize_t n = ::write(fd, content.data() + off, content.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false; else off += static_cast<size_t>(n);
    }
    ok = (::fsync(fd) == 0) && ok;
    ::close(fd);
    if (ok && ::rename(tmpl.c_str(), path.c_str()) == 0) return true;
    ::unlink(tmpl.c_str());
    return false;
}

}  // namespace fbx::xserver::userfiles
