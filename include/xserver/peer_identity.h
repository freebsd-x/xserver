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

// Is `exe` one of the desktop's own binaries named `name` — built (./, ./out/)
// or installed (/usr/local/bin/)?
inline bool isBuiltin(const std::string& exe, const std::string& name) {
    if (exe.empty()) return false;
    for (const std::string& cand : {"./" + name, "./out/" + name, "./out/" + name + "_app",
                                    "/usr/local/bin/" + name}) {
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
