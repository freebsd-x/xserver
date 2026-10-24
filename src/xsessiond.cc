// xsessiond — unified seat + session daemon (phase 1: authentication).
//
// See core/xserver/docs/xsessiond-design.md. Phase 1 hosts the PAM check in a
// persistent root daemon so the unprivileged compositor can verify a password
// without a per-check setuid exec (it supersedes xauthcheck; that helper
// stays as a fallback). Phase 2a adds device brokering (fd passing over
// SCM_RIGHTS); later phases add VT switching and session activation behind the
// same socket.
//
// Model: a root daemon listens on an AF_UNIX SOCK_SEQPACKET socket and
// multiplexes all connections in a single process with a poll() event loop,
// holding one Session per connection fd. (It began as accept-fork — one child
// per connection — but phase 2b needs a *single* process that owns the seat and
// the active-session set so it can drop/reacquire DRM master across sessions on
// a VT switch; that can't work with isolated per-connection children. The
// event-loop groundwork landed ahead of the live-seat DRM/VT work.) Each
// message is processed non-blocking-ish per readable fd; a slow PAM stack can
// briefly stall the loop — acceptable for the current single-seat reality, and
// session *spawn* (phase 1c) will fork+exec rather than block. Runs as root; the
// compositor and everything above it stay unprivileged.

#include "xserver/xsessiond_protocol.h"
#include "xserver/priv_ops.h"

#include <security/pam_appl.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <utmpx.h>
#include <vector>

extern "C" {
#include <libseat.h>   // phase 2b (opt-in): run as a libseat client of real seatd
}

extern char** environ;

namespace {

using namespace fbx::xserver::xsessiond;

// Device-node allowlist (phase 2a). A brokered OpenDevice may only hand out fds
// for real seat devices, never arbitrary files — the path is realpath()'d and
// must sit under one of these prefixes. Defaults to the DRM + evdev dirs; a
// test/dev override (XSESSIOND_DEV_PREFIX, read before env scrub) replaces them
// and relaxes the char-device requirement so the fd-passing path can be
// exercised headlessly against a scratch file. Set once in main().
std::vector<std::string> g_dev_prefixes;
bool g_dev_strict = true;   // require S_ISCHR on opened nodes (off in test mode)

// Phase 2b libseat-client state (opt-in, XSESSIOND_LIBSEAT). Declared early so
// session_teardown() can release seatd leases. Default off — g_libseat stays
// null and every libseat branch is skipped, preserving the phase-2a behavior.
libseat* g_libseat        = nullptr;
bool     g_libseat_active = false;
int      g_disable_pending = 0;   // outstanding DisableAcks before we ack seatd

// Setuid/daemon hygiene: never trust an inherited environment (PATH, IFS, LD_*,
// locale) — PAM and libc may consult it. Rebuild a minimal, fixed one.
void scrub_environment() {
    static char* empty[] = {nullptr};
    environ = empty;
    setenv("PATH", "/usr/bin:/bin:/usr/local/bin", 1);
}

// Per-connection state: a connection carries exactly one session. Authenticate
// creates the PAM handle and (on success) keeps it open so a following
// StartSession can open the PAM session + write the utmpx login record. The
// handle is torn down when the connection closes (see handle_connection).
struct Session {
    pam_handle_t* pamh = nullptr;
    std::string   pw;                 // backing store for the conv; cleared post-auth
    std::string   user;
    bool          authed = false;
    bool          open   = false;
    char          ut_id[8] = {};
    pid_t         leader = 0;
    // Devices leased to this connection (id → daemon-side fd). Closed on
    // CloseDevice or connection teardown. The peer holds its own SCM_RIGHTS copy.
    std::map<int,int> devices;
    int           next_dev = 1;
    // libseat mode only (XSESSIOND_LIBSEAT): our device id → seatd's device id,
    // so CloseDevice/teardown can libseat_close_device the right lease.
    std::map<int,int> libseat_ids;
};

constexpr const char* kUtLine = "xsess0";  // synthetic line for the GUI seat

// Answer every PAM prompt with the connection's stored password (same shape as
// authcheck.cc / polkit-agent-helper-1). After authentication pw is cleared, so
// any later session-module prompt simply receives an empty string.
int conv_fn(int num_msg, const struct pam_message** msg,
            struct pam_response** resp, void* appdata) {
    (void)msg;
    if (num_msg <= 0) return PAM_CONV_ERR;
    auto* s = static_cast<Session*>(appdata);
    auto* r = static_cast<struct pam_response*>(
        calloc(static_cast<size_t>(num_msg), sizeof(struct pam_response)));
    if (!r) return PAM_BUF_ERR;
    for (int i = 0; i < num_msg; ++i) {
        r[i].resp_retcode = 0;
        r[i].resp = strdup(s->pw.c_str());
    }
    *resp = r;
    return PAM_SUCCESS;
}

// pam_start + pam_authenticate + pam_acct_mgmt. On success the PAM handle is
// retained (for StartSession); on failure it is ended. Rejects expired/locked
// accounts even with a correct password. The password is scrubbed either way.
bool session_authenticate(Session& s, const std::string& user, const std::string& pass) {
    if (s.pamh) { pam_end(s.pamh, PAM_SUCCESS); s.pamh = nullptr; s.authed = false; }
    s.pw = pass;
    struct pam_conv conv{conv_fn, &s};   // PAM copies fn+appdata (&s stays valid)
    if (pam_start("freebsdx", user.c_str(), &conv, &s.pamh) != PAM_SUCCESS) {
        s.pamh = nullptr;
        std::fill(s.pw.begin(), s.pw.end(), '\0'); s.pw.clear();
        return false;
    }
    int ret = pam_authenticate(s.pamh, 0);
    if (ret == PAM_SUCCESS) ret = pam_acct_mgmt(s.pamh, 0);
    std::fill(s.pw.begin(), s.pw.end(), '\0'); s.pw.clear();
    if (ret != PAM_SUCCESS) { pam_end(s.pamh, ret); s.pamh = nullptr; return false; }
    s.user = user;
    s.authed = true;
    return true;
}

// Write a utmpx USER_PROCESS record so who(1)/w(1)/last(1) see the GUI login.
void utmpx_login(Session& s) {
    struct utmpx u{};
    u.ut_type = USER_PROCESS;
    gettimeofday(&u.ut_tv, nullptr);
    std::strncpy(u.ut_user, s.user.c_str(), sizeof(u.ut_user) - 1);
    std::strncpy(u.ut_line, kUtLine, sizeof(u.ut_line) - 1);
    std::snprintf(s.ut_id, sizeof(s.ut_id), "%x",
                  static_cast<unsigned>(s.leader & 0xffffff));
    std::strncpy(u.ut_id, s.ut_id, sizeof(u.ut_id));
    u.ut_pid = s.leader;
    setutxent(); pututxline(&u); endutxent();
}

void utmpx_logout(Session& s) {
    struct utmpx u{};
    u.ut_type = DEAD_PROCESS;
    gettimeofday(&u.ut_tv, nullptr);
    std::strncpy(u.ut_id, s.ut_id, sizeof(u.ut_id));  // matched by id
    std::strncpy(u.ut_line, kUtLine, sizeof(u.ut_line) - 1);
    u.ut_pid = s.leader;
    setutxent(); pututxline(&u); endutxent();
}

// Establish credentials, open the PAM session, and record the login. Requires a
// prior successful Authenticate on this connection.
bool session_open(Session& s, pid_t leader) {
    if (!s.authed || !s.pamh || s.open) return false;
    s.leader = leader;
    (void)pam_setcred(s.pamh, PAM_ESTABLISH_CRED);
    if (pam_open_session(s.pamh, 0) != PAM_SUCCESS) {
        pam_setcred(s.pamh, PAM_DELETE_CRED);
        return false;
    }
    s.open = true;
    utmpx_login(s);
    return true;
}

void session_close(Session& s) {
    if (!s.open) return;
    utmpx_logout(s);
    pam_close_session(s.pamh, 0);
    pam_setcred(s.pamh, PAM_DELETE_CRED);
    s.open = false;
}

// Full teardown when the connection ends (explicit EndSession or disconnect).
void session_teardown(Session& s) {
    session_close(s);
    if (g_libseat) {                                  // release seatd leases first
        for (auto& [id, ls] : s.libseat_ids) ::libseat_close_device(g_libseat, ls);
        s.libseat_ids.clear();
    }
    for (auto& [id, dfd] : s.devices) ::close(dfd);   // release leased devices
    s.devices.clear();
    if (s.pamh) { pam_end(s.pamh, PAM_SUCCESS); s.pamh = nullptr; }
    s.authed = false;
}

// Perform one allowlisted privileged op as root. The caller MUST have passed
// privops::validateOp(verb, arg) first — this re-checks the verb and acts only
// on known ones. No shell, no arbitrary exec: a fixed file write / sysctl per
// verb. Returns 0 on success or a negative errno.
int32_t perform_privileged(const std::string& verb, const std::string& arg) {
    using namespace fbx::xserver::privops;
    if (verb == kResolvConfWrite) {
        int wfd = ::open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (wfd < 0) return -errno;
        ssize_t w = ::write(wfd, arg.data(), arg.size());
        ::close(wfd);
        return (w == static_cast<ssize_t>(arg.size())) ? 0 : -EIO;
    }
    if (verb == kAudioDefaultUnit) {
        int unit = ::atoi(arg.c_str());
        if (::sysctlbyname("hw.snd.default_unit", nullptr, nullptr, &unit, sizeof(unit)) != 0)
            return -errno;
        return 0;
    }
    if (verb == kFirewallEnable) {
        // FIXED argv — the (validated "0"/"1") arg only selects -d / -e; it never
        // becomes part of the command line. posix_spawn, no shell.
        const char* flag = (arg == "1") ? "-e" : "-d";
        char* const argv[] = { const_cast<char*>("pfctl"), const_cast<char*>(flag), nullptr };
        pid_t pid = 0;
        int rc = ::posix_spawn(&pid, "/sbin/pfctl", nullptr, nullptr, argv, environ);
        if (rc != 0) return -rc;
        int status = 0;
        if (::waitpid(pid, &status, 0) < 0) return -errno;
        return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -EIO;
    }
    if (verb == kKmodUnload) {
        // `kldunload -n <name>`. The name is validated ([A-Za-z0-9._-], alnum
        // first) and passed as its own argv element after -n, so it can be
        // neither a flag nor a shell injection.
        char* const argv[] = { const_cast<char*>("kldunload"), const_cast<char*>("-n"),
                               const_cast<char*>(arg.c_str()), nullptr };
        pid_t pid = 0;
        int rc = ::posix_spawn(&pid, "/sbin/kldunload", nullptr, nullptr, argv, environ);
        if (rc != 0) return -rc;
        int status = 0;
        if (::waitpid(pid, &status, 0) < 0) return -errno;
        return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -EIO;
    }
    return -EINVAL;
}

void send_reply(int fd, uint32_t serial, Reply reply, int32_t result) {
    ReplyHeader h{};
    h.magic  = kMagic;
    h.version = kVersion;
    h.reply  = static_cast<uint16_t>(reply);
    h.serial = serial;
    h.result = result;
    // MSG_EOR delimits the SEQPACKET record; without it FreeBSD coalesces
    // back-to-back replies into one recvmsg (see IpcSocket::sendMessage).
    (void)::send(fd, &h, sizeof(h), MSG_NOSIGNAL | MSG_EOR);
}

// Send a Device reply carrying an open fd in the ancillary SCM_RIGHTS buffer.
// The peer receives its own copy of the descriptor. result = the device id.
void send_device_fd(int fd, uint32_t serial, int32_t id, int passfd) {
    ReplyHeader h{};
    h.magic  = kMagic;
    h.version = kVersion;
    h.reply  = static_cast<uint16_t>(Reply::Device);
    h.serial = serial;
    h.result = id;

    struct iovec io{ &h, sizeof(h) };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } u{};
    struct msghdr m{};
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = u.buf;
    m.msg_controllen = sizeof(u.buf);
    struct cmsghdr* c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type  = SCM_RIGHTS;
    c->cmsg_len   = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(c), &passfd, sizeof(int));
    // MSG_EOR delimits the SEQPACKET record (carries the SCM_RIGHTS fd with it);
    // without it FreeBSD coalesces this with adjacent replies (see sendMessage).
    (void)::sendmsg(fd, &m, MSG_NOSIGNAL | MSG_EOR);
}

// Canonicalise + allowlist-check a device path. Returns the resolved absolute
// path on success; on failure returns "" and sets err to a negative errno. The
// path must resolve (realpath, so symlinks/".." can't escape) under an allowed
// prefix, so a caller can't coax the root daemon into opening, say,
// /etc/master.passwd. Shared by the raw-open and libseat-client paths.
std::string device_resolve(const std::string& path, int& err) {
    err = 0;
    if (path.empty() || path.size() >= PATH_MAX) { err = -EINVAL; return {}; }
    char resolved[PATH_MAX];
    if (!::realpath(path.c_str(), resolved)) { err = -(errno ? errno : EINVAL); return {}; }
    std::string rp(resolved);
    for (const auto& pre : g_dev_prefixes)
        if (rp.size() >= pre.size() && rp.compare(0, pre.size(), pre) == 0) return rp;
    err = -EACCES;
    return {};
}

// Open an allowlisted device node directly (default / phase-2a path). Returns an
// open fd, or a negative errno. In strict (production) mode the target must be a
// char device (DRM/evdev); the test override relaxes that to pass a scratch file.
int device_open(const std::string& path) {
    int err = 0;
    std::string rp = device_resolve(path, err);
    if (rp.empty()) return err;
    // O_NONBLOCK is required: libinput opens evdev nodes non-blocking (it passes
    // O_NONBLOCK in the open_restricted flags) and reads them edge-triggered — a
    // blocking evdev fd makes libinput_udev_assign_seat()/dispatch() hang on the
    // first read with no pending events, which stalls the whole compositor startup.
    // DRM fds are also fine non-blocking (page-flip events are read edge-triggered
    // from the event loop). This matches what seatd/libseat hand out. The flags the
    // client's open_restricted requested are lost across the IPC, so apply the seat
    // daemon's standard set here.
    int dfd = ::open(rp.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (dfd < 0) return -(errno ? errno : EACCES);
    if (g_dev_strict) {
        struct stat st{};
        if (::fstat(dfd, &st) != 0 || !S_ISCHR(st.st_mode)) {
            ::close(dfd);
            return -EACCES;
        }
    }
    return dfd;
}

// --- Phase 2b: libseat-client mode (opt-in, XSESSIOND_LIBSEAT) ---------------
// When enabled the daemon is itself a libseat client of real seatd, so brokered
// DRM fds carry proper master and seatd's VT-switch activation can be relayed to
// the compositor. Default OFF (g_libseat stays null → device_open() raw path,
// today's exact behavior). This path is compile-verified but must be validated
// on the live seat (Stage C) — it can't run headlessly (opening a seat grabs the
// seat). See docs/xsessiond-design.md §4b, §6. (g_libseat/g_libseat_active/
// g_disable_pending are declared earlier so session_teardown() can see them.)
std::unordered_map<int, Session>* g_sessions = nullptr;  // set in main(), for callbacks

// libseat-client open: seatd opens the (already allowlisted) node and returns a
// properly-mastered fd. Returns fd (>=0) and sets libseat_id, or a negative errno.
int device_open_seatd(const std::string& path, int& libseat_id) {
    libseat_id = -1;
    int err = 0;
    std::string rp = device_resolve(path, err);
    if (rp.empty()) return err;
    int fd = -1;
    int id = ::libseat_open_device(g_libseat, rp.c_str(), &fd);
    if (id < 0 || fd < 0) { if (fd >= 0) ::close(fd); return -(errno ? errno : EACCES); }
    libseat_id = id;
    return fd;
}

// Push an unsolicited SeatDisable/SeatEnable to every connection that currently
// holds a leased device (i.e. the compositor owning the seat). Returns the count
// notified, which bounds how many DisableAcks we then wait for.
int push_activation(Reply which) {
    if (!g_sessions) return 0;
    int n = 0;
    for (auto& [cfd, s] : *g_sessions) {
        if (s.devices.empty()) continue;
        ReplyHeader h{};
        h.magic = kMagic; h.version = kVersion;
        h.reply = static_cast<uint16_t>(which); h.serial = kPushSerial; h.result = 0;
        // MSG_EOR delimits the SEQPACKET record (see IpcSocket::sendMessage).
        (void)::send(cfd, &h, sizeof(h), MSG_NOSIGNAL | MSG_EOR);
        ++n;
    }
    return n;
}

void on_libseat_enable(libseat* /*seat*/, void* /*data*/) {
    g_libseat_active = true;
    push_activation(Reply::SeatEnable);
}

void on_libseat_disable(libseat* seat, void* /*data*/) {
    g_libseat_active = false;
    // Tell the compositor(s) to quiesce the GPU; only ack the disable down to
    // seatd once they confirm (DisableAck) so DRM master isn't yanked mid-flip.
    g_disable_pending = push_activation(Reply::SeatDisable);
    if (g_disable_pending == 0) ::libseat_disable_seat(seat);   // nobody to wait on
}

libseat_seat_listener g_libseat_listener = {
    .enable_seat  = on_libseat_enable,
    .disable_seat = on_libseat_disable,
};

// Process one received datagram for a connection. Returns false if the
// connection should be closed (bad framing → fail closed); true to keep it open.
// The event loop owns the recv() and the teardown; this is pure per-message
// dispatch over the connection's Session (a stable reference into the session
// map — see main()).
bool dispatch_datagram(int fd, Session& s, const unsigned char* buf, size_t n) {
    if (n < sizeof(RequestHeader)) return false;

    RequestHeader hdr{};
    std::memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != kMagic || hdr.version != kVersion) {
        send_reply(fd, hdr.serial, Reply::Error, -EINVAL);
        return false;
    }

    switch (static_cast<Opcode>(hdr.opcode)) {
    case Opcode::Hello:
        send_reply(fd, hdr.serial, Reply::Welcome,
                   kCapAuthenticate | kCapSession | kCapDevice | kCapPrivileged |
                   (g_libseat ? kCapActivation : 0));
        break;

    case Opcode::Authenticate: {
        // Bounds-check the two trailing fields against the datagram we got.
        if (hdr.field1_len > kMaxField || hdr.field2_len > kMaxField) {
            send_reply(fd, hdr.serial, Reply::Error, -EINVAL);
            break;
        }
        const size_t need = sizeof(RequestHeader) +
                            static_cast<size_t>(hdr.field1_len) + hdr.field2_len;
        if (need > n) {
            send_reply(fd, hdr.serial, Reply::Error, -EINVAL);
            break;
        }
        const unsigned char* p = buf + sizeof(RequestHeader);
        std::string user(reinterpret_cast<const char*>(p), hdr.field1_len);
        std::string pass(reinterpret_cast<const char*>(p + hdr.field1_len),
                         hdr.field2_len);
        bool ok = !user.empty() && session_authenticate(s, user, pass);
        // Scrub the secret from our scratch buffer promptly.
        std::memset(pass.data(), 0, pass.size());
        send_reply(fd, hdr.serial, Reply::AuthResult, ok ? 1 : 0);
        break;
    }

    case Opcode::StartSession: {
        // field1 (optional) = 4-byte session-leader pid for utmpx accounting.
        pid_t leader = 0;
        if (hdr.field1_len == sizeof(uint32_t) &&
            sizeof(RequestHeader) + sizeof(uint32_t) <= n) {
            uint32_t v = 0;
            std::memcpy(&v, buf + sizeof(RequestHeader), sizeof(v));
            leader = static_cast<pid_t>(v);
        }
        bool ok = session_open(s, leader);
        send_reply(fd, hdr.serial, Reply::SessionResult, ok ? 1 : 0);
        break;
    }

    case Opcode::EndSession:
        session_close(s);
        send_reply(fd, hdr.serial, Reply::SessionResult, 1);
        break;

    case Opcode::OpenDevice: {
        if (hdr.field1_len == 0 || hdr.field1_len > kMaxField ||
            sizeof(RequestHeader) + hdr.field1_len > n) {
            send_reply(fd, hdr.serial, Reply::Device, -EINVAL);
            break;
        }
        std::string path(reinterpret_cast<const char*>(buf + sizeof(RequestHeader)),
                         hdr.field1_len);
        int ls_id = -1;
        int dfd = g_libseat ? device_open_seatd(path, ls_id) : device_open(path);
        if (dfd < 0) {                             // negative errno, no fd
            send_reply(fd, hdr.serial, Reply::Device, dfd);
            break;
        }
        int id = s.next_dev++;
        s.devices[id] = dfd;
        if (g_libseat) s.libseat_ids[id] = ls_id;
        send_device_fd(fd, hdr.serial, id, dfd);
        break;
    }

    case Opcode::CloseDevice: {
        int id = -1;
        if (hdr.field1_len == sizeof(uint32_t) &&
            sizeof(RequestHeader) + sizeof(uint32_t) <= n) {
            uint32_t v = 0;
            std::memcpy(&v, buf + sizeof(RequestHeader), sizeof(v));
            id = static_cast<int>(v);
        }
        auto it = s.devices.find(id);
        if (it != s.devices.end()) {
            if (auto li = s.libseat_ids.find(id); li != s.libseat_ids.end()) {
                ::libseat_close_device(g_libseat, li->second);  // tell seatd
                s.libseat_ids.erase(li);
            }
            ::close(it->second);                   // our own fd copy
            s.devices.erase(it);
            send_reply(fd, hdr.serial, Reply::DeviceClosed, 1);
        } else {
            send_reply(fd, hdr.serial, Reply::DeviceClosed, 0);
        }
        break;
    }

    case Opcode::DisableAck:
        // The compositor has quiesced the GPU. Once every notified session has
        // acked, hand DRM master back to seatd to let the VT switch complete.
        // No reply — this only unblocks the daemon-side disable handshake.
        if (g_disable_pending > 0 && --g_disable_pending == 0 && g_libseat)
            ::libseat_disable_seat(g_libseat);
        break;

    case Opcode::RunPrivilegedOp: {
        // TRUST MODEL (phase 3): the gate is privops::validateOp (fixed verbs +
        // validated args — no arbitrary command) plus the socket's permissions
        // (xsessiond.sock must be reachable only by the seat user). Requiring the
        // connection to be authenticated first is a hardening follow-up (the
        // compositor uses a fresh one-shot connection here).
        if (hdr.field1_len > kMaxField || hdr.field2_len > kMaxField) {
            send_reply(fd, hdr.serial, Reply::Error, -EINVAL);
            break;
        }
        const size_t need = sizeof(RequestHeader) +
                            static_cast<size_t>(hdr.field1_len) + hdr.field2_len;
        if (need > n) { send_reply(fd, hdr.serial, Reply::Error, -EINVAL); break; }
        const unsigned char* p = buf + sizeof(RequestHeader);
        std::string verb(reinterpret_cast<const char*>(p), hdr.field1_len);
        std::string arg(reinterpret_cast<const char*>(p + hdr.field1_len), hdr.field2_len);
        if (!fbx::xserver::privops::validateOp(verb, arg)) {
            send_reply(fd, hdr.serial, Reply::PrivilegedResult, -EINVAL);  // refuse
            break;
        }
        send_reply(fd, hdr.serial, Reply::PrivilegedResult, perform_privileged(verb, arg));
        break;
    }

    default:
        send_reply(fd, hdr.serial, Reply::Error, -ENOSYS);
        break;
    }
    return true;
}

std::string socket_path() {
    if (const char* env = ::getenv(kSocketPathEnv); env && env[0]) return env;
    return kDefaultSocketPath;
}

} // namespace

int main() {
    // Read env-provided config (socket path, device allowlist) BEFORE scrubbing
    // the environment — scrub_environment() wipes environ, so any getenv()
    // afterwards sees nothing.
    const std::string path = socket_path();
    if (const char* p = ::getenv("XSESSIOND_DEV_PREFIX"); p && p[0]) {
        // Test/dev override: broker devices from this prefix, non-strict so a
        // scratch file can be passed for headless fd-passing tests.
        g_dev_prefixes = { p };
        g_dev_strict = false;
    } else {
        // FreeBSD DRM nodes live under /dev/drm/ (e.g. /dev/drm/0); /dev/dri/cardN
        // and /dev/dri/renderDN are compat symlinks into it. Since device_resolve()
        // realpath()s before the prefix check, a request for /dev/dri/card0 resolves
        // to /dev/drm/0 — so /dev/drm/ MUST be on the allowlist or every DRM open is
        // refused with EACCES. /dev/dri/ is kept for Linux layouts / render nodes
        // that don't symlink out. /dev/input/ is evdev. (See the device-layout note
        // in docs/xsessiond-design.md §4a.)
        g_dev_prefixes = { "/dev/drm/", "/dev/dri/", "/dev/input/" };
        g_dev_strict = true;
    }
    // Phase 2b opt-in (read before scrub wipes environ). Enabling this makes the
    // daemon a libseat client of real seatd; leave it unset for today's behavior.
    const bool want_libseat = ::getenv("XSESSIOND_LIBSEAT") != nullptr;
    scrub_environment();

    // Single process now (no per-connection fork), so no SIGCHLD reaping. Ignore
    // SIGPIPE so a client vanishing mid-reply can't take the daemon down.
    ::signal(SIGPIPE, SIG_IGN);

    int lfd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (lfd < 0) { std::perror("xsessiond: socket"); return 1; }

    ::unlink(path.c_str());  // clear a stale socket from a previous run
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        std::fprintf(stderr, "xsessiond: socket path too long: %s\n", path.c_str());
        return 1;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::perror("xsessiond: bind");
        return 1;
    }
    // World-connectable: any local user may authenticate *as themselves*; PAM is
    // the gate. Peer-credential authorization (per-session ownership) arrives
    // with the session opcodes in a later phase — see design §8.
    ::chmod(path.c_str(), 0666);

    if (::listen(lfd, 16) != 0) { std::perror("xsessiond: listen"); return 1; }

    // fd → per-connection Session. unordered_map keeps element *addresses* stable
    // across insert/rehash, which matters: session_authenticate() stashes
    // &Session as PAM's conv appdata, so the node must not move under it.
    std::unordered_map<int, Session> sessions;
    g_sessions = &sessions;   // the libseat activation callbacks push to it

    // Phase 2b: become a libseat client of real seatd (opt-in). The initial
    // enable_seat callback fires during the priming dispatch loop and flips
    // g_libseat_active. Fatal if requested but unavailable — the operator asked
    // for the brokered-master path explicitly.
    if (want_libseat) {
        g_libseat = ::libseat_open_seat(&g_libseat_listener, nullptr);
        if (!g_libseat) { std::perror("xsessiond: libseat_open_seat"); return 1; }
        for (int i = 0; i < 100 && !g_libseat_active; ++i) {
            if (::libseat_dispatch(g_libseat, 20) < 0) {
                std::perror("xsessiond: libseat_dispatch (priming)");
                return 1;
            }
        }
        if (!g_libseat_active) {
            std::fprintf(stderr, "xsessiond: seat never activated\n");
            return 1;
        }
        std::fprintf(stderr, "xsessiond: libseat-client mode active (seat brokered)\n");
    }

    // Close a connection: PAM/session teardown (utmpx logout, close leased
    // devices), drop from the map, close the fd.
    auto drop = [&](int fd) {
        if (auto it = sessions.find(fd); it != sessions.end()) {
            session_teardown(it->second);
            sessions.erase(it);
        }
        ::close(fd);
    };

    // Layout of pfds each pass: [0] = listen fd; [1] = libseat fd (only in
    // libseat mode); then one entry per connection. first_conn marks where the
    // connection entries start so the serve loop never treats the listen/seat fd
    // as a connection (dropping the seat fd would close libseat's socket).
    const int  seat_fd    = g_libseat ? ::libseat_get_fd(g_libseat) : -1;
    const size_t first_conn = (seat_fd >= 0) ? 2 : 1;

    std::vector<struct pollfd> pfds;
    for (;;) {
        pfds.clear();
        pfds.push_back(pollfd{ lfd, POLLIN, 0 });
        if (seat_fd >= 0) pfds.push_back(pollfd{ seat_fd, POLLIN, 0 });
        for (const auto& [cfd, s] : sessions) pfds.push_back(pollfd{ cfd, POLLIN, 0 });

        if (::poll(pfds.data(), pfds.size(), -1) < 0) {
            if (errno == EINTR) continue;
            std::perror("xsessiond: poll");
            continue;
        }

        // Pump seatd first: libseat_dispatch runs the enable/disable callbacks,
        // which may push activation to the connections we serve below.
        if (seat_fd >= 0 && (pfds[1].revents & POLLIN)) {
            if (::libseat_dispatch(g_libseat, 0) < 0)
                std::perror("xsessiond: libseat_dispatch");
        }

        // Accept new connections first.
        if (pfds[0].revents & POLLIN) {
            int cfd = ::accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
            if (cfd >= 0) {
                sessions.emplace(cfd, Session{});   // fresh per-connection state
            } else if (errno != EINTR) {
                std::perror("xsessiond: accept");
            }
        }

        // Serve existing connections. Iterate the pfds snapshot (not the map),
        // so a drop() mid-pass is safe; indices below first_conn are the listen
        // (and, in libseat mode, seat) fds handled above. A connection accepted
        // this pass isn't in the snapshot — it waits for the next poll, fine.
        for (size_t i = first_conn; i < pfds.size(); ++i) {
            const short re = pfds[i].revents;
            if (re == 0) continue;
            const int cfd = pfds[i].fd;
            if (re & (POLLERR | POLLHUP | POLLNVAL)) { drop(cfd); continue; }
            if (!(re & POLLIN)) continue;

            auto it = sessions.find(cfd);
            if (it == sessions.end()) continue;     // already dropped this pass

            unsigned char buf[kMaxMessage];
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0 ||
                !dispatch_datagram(cfd, it->second, buf, static_cast<size_t>(n))) {
                drop(cfd);                          // EOF / error / bad framing
            }
        }
    }
}
