#pragma once

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

// A minimal client for wpa_supplicant's control interface — the same text
// protocol wpa_cli speaks, without running wpa_cli: no process, no shell, and
// no secret (a PSK) on a command line any local user can read with ps.
// One request, one reply. Needs access to /var/run/wpa_supplicant/<iface>
// (the ctrl_interface group — wheel by default).
namespace fbx::xserver::wpa {

inline bool validIface(const std::string& iface) {
    if (iface.empty() || iface.size() > 15) return false;
    for (char c : iface)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

// Send `cmd` to wpa_supplicant on `iface`; its reply ("" on error/timeout).
inline std::string request(const std::string& iface, const std::string& cmd, int timeout_ms = 3000) {
    if (!validIface(iface)) return {};
    int s = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return {};
    // The reply goes to our own bound address: a private, unique path.
    static std::atomic<unsigned> seq{0};
    sockaddr_un local{};
    local.sun_family = AF_UNIX;
    std::snprintf(local.sun_path, sizeof(local.sun_path), "/tmp/fbx-wpa-%d-%u",
                  static_cast<int>(::getpid()), seq.fetch_add(1));
    ::unlink(local.sun_path);
    sockaddr_un dest{};
    dest.sun_family = AF_UNIX;
    std::snprintf(dest.sun_path, sizeof(dest.sun_path), "/var/run/wpa_supplicant/%s", iface.c_str());
    std::string reply;
    if (::bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0) {
        if (::connect(s, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) == 0 &&
            ::send(s, cmd.data(), cmd.size(), 0) == static_cast<ssize_t>(cmd.size())) {
            char buf[4096];
            for (int tries = 0; tries < 8; ++tries) {
                pollfd p{s, POLLIN, 0};
                if (::poll(&p, 1, timeout_ms) <= 0) break;
                ssize_t n = ::recv(s, buf, sizeof(buf), 0);
                if (n <= 0) break;
                if (buf[0] == '<') continue;   // an unsolicited event ("<3>CTRL-EVENT-…"), not our reply
                reply.assign(buf, static_cast<size_t>(n));
                break;
            }
        }
        ::unlink(local.sun_path);
    }
    ::close(s);
    return reply;
}

}  // namespace fbx::xserver::wpa
