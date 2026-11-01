#pragma once

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <string>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/ucred.h>
#include <sys/un.h>
#include <vector>

// Who is on the other end of a client socket — by the executable it runs, not
// by anything the client says about itself (XAPP_ID is just an environment
// variable). The peer's pid comes from LOCAL_PEERCRED, its binary from
// kern.proc.pathname. A Capsicum-sandboxed app can't exec another binary, so
// it can't pass as a different one; a same-user unsandboxed process could
// already ptrace any of them, so this is the boundary that matters.
namespace fbx::xserver::peer {

// The real path of the peer's executable, or "" if unknown.
inline std::string executable(int fd) {
    struct xucred xu{};
    socklen_t len = sizeof(xu);
    if (::getsockopt(fd, 0 /* SOL_LOCAL */, LOCAL_PEERCRED, &xu, &len) != 0 ||
        xu.cr_version != XUCRED_VERSION || xu.cr_pid <= 0)
        return {};
    char path[PATH_MAX] = {};
    size_t plen = sizeof(path) - 1;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, xu.cr_pid};
    if (::sysctl(mib, 4, path, &plen, nullptr, 0) != 0) return {};
    char real[PATH_MAX];   // e.g. /home → /usr/home on FreeBSD
    return ::realpath(path, real) ? std::string(real) : std::string(path);
}

// The directory of this process's own executable (with a trailing '/'), or "".
inline const std::string& ownDirectory() {
    static const std::string dir = [] {
        char path[PATH_MAX] = {};
        size_t plen = sizeof(path) - 1;
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
        if (::sysctl(mib, 4, path, &plen, nullptr, 0) != 0) return std::string();
        char real[PATH_MAX];
        std::string p = ::realpath(path, real) ? std::string(real) : std::string(path);
        auto slash = p.rfind('/');
        return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
    }();
    return dir;
}

// Is `exe` one of the desktop's own binaries named `name` — built (./, ./out/,
// or next to the compositor itself, e.g. a sanitizer build in out-asan/) or
// installed (/usr/local/bin/)? Trusting the compositor's own directory adds
// nothing: whoever can write there can already replace the compositor.
inline bool isBuiltin(const std::string& exe, const std::string& name) {
    if (exe.empty()) return false;
    const std::string& own = ownDirectory();
    for (const std::string& cand : {"./" + name, "./out/" + name, "./out/" + name + "_app",
                                    own.empty() ? std::string() : own + name,
                                    own.empty() ? std::string() : own + name + "_app",
                                    "/usr/local/bin/" + name}) {
        if (cand.empty()) continue;
        char real[PATH_MAX];
        if (::realpath(cand.c_str(), real) && exe == real) return true;
    }
    return false;
}

// The peer is one of the given built-in binaries.
inline bool isOneOf(int fd, std::initializer_list<const char*> names) {
    const std::string exe = executable(fd);
    return std::any_of(names.begin(), names.end(), [&](const char* n) { return isBuiltin(exe, n); });
}

}  // namespace fbx::xserver::peer
