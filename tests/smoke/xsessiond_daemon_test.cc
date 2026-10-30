// Smoke test for the xsessiond daemon — spawns the real built binary on a
// scratch socket and drives the real client code against it. Self-contained: it
// fork+execs xsessiond (path passed as argv[1] by meson), points it at a temp
// socket + temp device-allowlist prefix, runs the checks, then tears it down.
//
// As an unprivileged uid PAM can't read the shadow DB and can't open a PAM
// session / write utmpx, so positive auth and a successful StartSession are NOT
// exercised here — what this verifies is the wire protocol, capability
// advertisement, single-process multiplexing (concurrent connections), the
// reject/gating paths, and device brokering incl. the SCM_RIGHTS fd round-trip.
// The positive PAM path is the live-seat tier (scripts/xsessiond-verify.sh).
//
// Plain exit-code test (0 = pass); no gtest, since it orchestrates a subprocess.

#include <sys/sysctl.h>
#include <climits>
#include "xserver/auth_helper.h"
#include "xserver/xsessiond_device.h"
#include "xserver/xsessiond_session.h"
#include "xserver/xsessiond_protocol.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>

using namespace fbx::xserver;
using namespace fbx::xserver::xsessiond;

namespace {

int   g_fails = 0;
pid_t g_daemon = -1;
std::string g_sock;
std::string g_devdir;

void check(const char* what, bool ok) {
    std::printf("  %-46s %s\n", what, ok ? "[OK]" : "[FAIL]");
    if (!ok) ++g_fails;
}

void cleanup() {
    if (g_daemon > 0) { ::kill(g_daemon, SIGTERM); ::waitpid(g_daemon, nullptr, 0); }
    if (!g_sock.empty())   ::unlink(g_sock.c_str());
}

int connect_sock() {
    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return -1;
    sockaddr_un a{}; a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, g_sock.c_str(), sizeof(a.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { ::close(fd); return -1; }
    return fd;
}

// One request datagram (header + optional field1), one reply; returns result or
// a sentinel on any error.
int32_t roundtrip(Opcode op, Reply expect, uint32_t f1len, const void* f1) {
    int fd = connect_sock();
    if (fd < 0) return -1000;
    RequestHeader h{}; h.magic = kMagic; h.version = kVersion;
    h.opcode = static_cast<uint16_t>(op); h.serial = 7; h.field1_len = f1len;
    std::string msg(reinterpret_cast<const char*>(&h), sizeof(h));
    if (f1len) msg.append(static_cast<const char*>(f1), f1len);
    ::send(fd, msg.data(), msg.size(), 0);
    ReplyHeader r{};
    ssize_t n = ::recv(fd, &r, sizeof(r), 0);
    ::close(fd);
    if (n != static_cast<ssize_t>(sizeof(r)) || r.reply != static_cast<uint16_t>(expect))
        return -1000;
    return r.result;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <path-to-xsessiond>\n", argv[0]); return 2; }
    const char* daemon_path = argv[1];

    // Temp socket (short path) + device-allowlist prefix with a scratch file.
    char sock[64], dir[64];
    std::snprintf(sock, sizeof(sock), "/tmp/fbxsessiond_%d.sock", static_cast<int>(getpid()));
    std::snprintf(dir,  sizeof(dir),  "/tmp/fbxsessiond_dev_%d/", static_cast<int>(getpid()));
    g_sock = sock; g_devdir = dir;
    ::mkdir(dir, 0700);
    std::string devfile = g_devdir + "brokered.dat";
    { int f = ::open(devfile.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600); if (f >= 0) ::close(f); }

    ::setenv(kSocketPathEnv, sock, 1);
    // The daemon serves only the compositor: name this test as it.
    {
        char self[PATH_MAX] = {};
        size_t len = sizeof(self) - 1;
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
        if (::sysctl(mib, 4, self, &len, nullptr, 0) == 0) ::setenv("XSESSIOND_COMPOSITOR", self, 1);
    }
    ::setenv("XSESSIOND_DEV_PREFIX", dir, 1);
    std::atexit(cleanup);

    // Spawn the daemon.
    g_daemon = ::fork();
    if (g_daemon == 0) {
        ::execl(daemon_path, daemon_path, static_cast<char*>(nullptr));
        std::perror("execl xsessiond");
        _exit(127);
    }
    if (g_daemon < 0) { std::perror("fork"); return 2; }

    // Wait up to ~3s for the socket to appear.
    bool up = false;
    for (int i = 0; i < 300; ++i) {
        struct stat st{};
        if (::stat(sock, &st) == 0 && S_ISSOCK(st.st_mode)) { up = true; break; }
        struct timespec ts{0, 10 * 1000 * 1000}; ::nanosleep(&ts, nullptr);
    }
    if (!up) { std::fprintf(stderr, "daemon did not come up on %s\n", sock); return 1; }

    std::printf("== xsessiond smoke (daemon pid %d) ==\n", static_cast<int>(g_daemon));

    // --- Protocol + capabilities ------------------------------------------------
    const int want_caps = kCapAuthenticate | kCapSession | kCapDevice | kCapPrivileged;
    int32_t caps = roundtrip(Opcode::Hello, Reply::Welcome, 0, nullptr);
    check("Hello advertises auth|session|device|privileged caps", caps == want_caps);

    // --- Single-process multiplexing: N concurrent connections ------------------
    {
        constexpr int N = 4;
        int fds[N];
        bool ok = true;
        for (int i = 0; i < N; ++i) fds[i] = connect_sock();
        for (int i = 0; i < N; ++i) {
            RequestHeader h{}; h.magic = kMagic; h.version = kVersion;
            h.opcode = static_cast<uint16_t>(Opcode::Hello); h.serial = 100 + i;
            if (fds[i] < 0 || ::send(fds[i], &h, sizeof(h), 0) != static_cast<ssize_t>(sizeof(h)))
                ok = false;
        }
        for (int i = 0; i < N; ++i) {
            ReplyHeader r{};
            ssize_t n = fds[i] >= 0 ? ::recv(fds[i], &r, sizeof(r), 0) : -1;
            if (n != static_cast<ssize_t>(sizeof(r)) ||
                r.serial != static_cast<uint32_t>(100 + i) || r.result != want_caps)
                ok = false;
            if (fds[i] >= 0) ::close(fds[i]);
        }
        check("4 concurrent connections each answered", ok);
    }

    // --- Privilege broker: the daemon refuses an unknown verb via validateOp ----
    {
        const char* verb = "definitely.not.allowed";
        int32_t res = roundtrip(Opcode::RunPrivilegedOp, Reply::PrivilegedResult,
                                static_cast<uint32_t>(std::strlen(verb)), verb);
        check("RunPrivilegedOp(unknown verb) refused (-EINVAL)", res == -EINVAL);
    }

    // --- Session gating + auth reject -------------------------------------------
    uint32_t pid = static_cast<uint32_t>(getpid());
    check("StartSession without auth is refused",
          roundtrip(Opcode::StartSession, Reply::SessionResult, sizeof(pid), &pid) == 0);
    check("authViaXsessiond(bad user) rejected",
          authViaXsessiond("no_such_user_zzq", "definitely-wrong") == 0);
    check("XsessiondSession::open(bad user) is nullptr",
          XsessiondSession::open("no_such_user_zzq", "wrong", getpid()) == nullptr);

    // --- Device brokering via the real ISeatManager client ----------------------
    {
        auto mgr = XsessiondDeviceManager::create();
        check("XsessiondDeviceManager::create (caps ok)", mgr.has_value());
        if (mgr) {
            auto dev = (*mgr)->openDevice(devfile);
            check("openDevice returns a device + fd",
                  dev.has_value() && dev->fd_.get() >= 0);
            if (dev) {
                const char* marker = "xsessiond-scm-rights";
                ssize_t w = ::write(dev->fd_.get(), marker, std::strlen(marker));
                check("write() through the brokered fd", w == (ssize_t)std::strlen(marker));
                char rb[64] = {};
                int rf = ::open(devfile.c_str(), O_RDONLY);
                ssize_t r = rf >= 0 ? ::read(rf, rb, sizeof(rb) - 1) : -1;
                if (rf >= 0) ::close(rf);
                check("bytes land in the underlying file",
                      r == w && std::strcmp(rb, marker) == 0);
                check("device id is valid (>0)", dev->device_id_ > 0);
                check("closeDevice(id) succeeds", (*mgr)->closeDevice(dev->device_id_).has_value());
                check("closeDevice(unknown id) refused", !(*mgr)->closeDevice(dev->device_id_).has_value());
            }
            check("openDevice(out-of-allowlist) refused",
                  !(*mgr)->openDevice("/etc/master.passwd").has_value());
        }
    }

    // --- Seat-activation push events (phase 2b) ---------------------------------
    // Drive SeatDisable/SeatEnable over a socketpair (no daemon needed): one end
    // is adopted by a XsessiondDeviceManager, the other plays the daemon relaying a
    // VT switch. Verifies dispatch() invokes the handlers and — critically — that
    // the DisableAck goes out only *after* the disable handler has run.
    {
        int sp[2];
        bool ok = ::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sp) == 0;
        check("socketpair for activation test", ok);
        if (ok) {
            auto mgr = XsessiondDeviceManager::adopt(sp[0]);   // takes sp[0]
            int seq = 0, disable_at = -1, enable_at = -1;
            bool disabled_before_ack = false;
            mgr->setActivationHandlers(
                [&] { disable_at = ++seq; },
                [&] { enable_at  = ++seq; });

            auto push = [&](Reply which) {
                ReplyHeader h{}; h.magic = kMagic; h.version = kVersion;
                h.reply = static_cast<uint16_t>(which); h.serial = kPushSerial;
                h.result = 0;
                ::send(sp[1], &h, sizeof(h), 0);
            };

            push(Reply::SeatDisable);
            auto d1 = mgr->dispatch();
            check("dispatch(SeatDisable) ok", d1.has_value());
            check("on_disable handler fired", disable_at == 1);

            // The daemon end must now see a DisableAck request.
            RequestHeader req{};
            ssize_t rn = ::recv(sp[1], &req, sizeof(req), MSG_DONTWAIT);
            bool got_ack = rn == (ssize_t)sizeof(req) && req.magic == kMagic &&
                           req.opcode == static_cast<uint16_t>(Opcode::DisableAck);
            check("DisableAck sent back to daemon", got_ack);
            disabled_before_ack = got_ack && disable_at == 1;
            check("handler ran before ack (ordering)", disabled_before_ack);

            push(Reply::SeatEnable);
            auto d2 = mgr->dispatch();
            check("dispatch(SeatEnable) ok", d2.has_value());
            check("on_enable handler fired", enable_at == 2);

            // Peer close is surfaced as an error so the loop can react.
            ::close(sp[1]);
            check("dispatch() reports lost daemon on peer close",
                  !mgr->dispatch().has_value());
        }
    }

    // --- Liveness ---------------------------------------------------------------
    check("daemon still alive after all connections", ::kill(g_daemon, 0) == 0);

    ::unlink(devfile.c_str());
    ::rmdir(g_devdir.c_str());
    std::printf("== %s ==\n", g_fails == 0 ? "SMOKE TEST PASSED" : "SMOKE TEST FAILED");
    return g_fails == 0 ? 0 : 1;
}
