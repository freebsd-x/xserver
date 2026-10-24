// Client side of xsessiond. Connects to the daemon's AF_UNIX SOCK_SEQPACKET
// socket to (a) verify a password one-shot (authViaXsessiond) and (b) hold a login
// session open for its lifetime (XsessiondSession). Runs in the unprivileged
// compositor; the daemon does the PAM/utmpx work as root. See
// docs/xsessiond-design.md and xserver/xsessiond_protocol.h.

#include "xserver/auth_helper.h"
#include "xserver/priv_broker.h"
#include "xserver/xsessiond_session.h"
#include "xserver/xsessiond_protocol.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace fbx::xserver {

using namespace fbx::xserver::xsessiond;

namespace {

std::string xsessiond_socket_path() {
    if (const char* env = ::getenv(kSocketPathEnv); env && env[0]) return env;
    return kDefaultSocketPath;
}

// Connect a fresh SEQPACKET socket to the daemon. Returns -1 if unreachable.
int xsessiond_connect() noexcept {
    const std::string path = xsessiond_socket_path();
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) return -1;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return -1;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Send one request datagram (header + up to two variable fields). The message
// buffer is scrubbed after sending so secrets don't linger. Returns false on a
// short/failed write.
bool xsessiond_send(int fd, Opcode op, uint32_t serial,
                const std::string& f1, const std::string& f2) noexcept {
    RequestHeader hdr{};
    hdr.magic      = kMagic;
    hdr.version    = kVersion;
    hdr.opcode     = static_cast<uint16_t>(op);
    hdr.serial     = serial;
    hdr.field1_len = static_cast<uint32_t>(f1.size());
    hdr.field2_len = static_cast<uint32_t>(f2.size());

    std::string msg;
    msg.reserve(sizeof(hdr) + f1.size() + f2.size());
    msg.append(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    msg.append(f1);
    msg.append(f2);

    // MSG_EOR delimits the SEQPACKET record; without it FreeBSD coalesces
    // back-to-back sends into one recvmsg (see IpcSocket::sendMessage).
    ssize_t w = ::send(fd, msg.data(), msg.size(), MSG_NOSIGNAL | MSG_EOR);
    std::memset(msg.data(), 0, msg.size());
    return w == static_cast<ssize_t>(msg.size());
}

// Read one reply datagram and check it matches the expected reply type.
// Returns the reply's result field, or a negative sentinel on any error.
constexpr int32_t kReplyError = -1000;
int32_t xsessiond_recv(int fd, Reply expect) noexcept {
    ReplyHeader rep{};
    ssize_t r = ::recv(fd, &rep, sizeof(rep), 0);
    if (r != static_cast<ssize_t>(sizeof(rep))) return kReplyError;
    if (rep.magic != kMagic || rep.version != kVersion) return kReplyError;
    if (rep.reply != static_cast<uint16_t>(expect)) return kReplyError;
    return rep.result;
}

} // namespace

int authViaXsessiond(const char* user, const char* password) noexcept {
    if (!user || !*user) return -1;
    const std::string user_s = user;
    const std::string pass_s = password ? password : "";
    if (user_s.size() > kMaxField || pass_s.size() > kMaxField) return -1;

    int fd = xsessiond_connect();
    if (fd < 0) return -1;  // daemon not running → caller falls back
    struct Closer { int fd; ~Closer() { ::close(fd); } } closer{fd};

    if (!xsessiond_send(fd, Opcode::Authenticate, 1, user_s, pass_s)) return -1;
    int32_t res = xsessiond_recv(fd, Reply::AuthResult);
    if (res == kReplyError) return -1;
    return res == 1 ? 1 : 0;
}

// ── XsessiondSession ──────────────────────────────────────────────────────────────

std::unique_ptr<XsessiondSession> XsessiondSession::open(const char* user,
                                                 const char* password,
                                                 pid_t leader) noexcept {
    if (!user || !*user) return nullptr;
    const std::string user_s = user;
    std::string pass_s = password ? password : "";
    if (user_s.size() > kMaxField || pass_s.size() > kMaxField) return nullptr;

    int fd = xsessiond_connect();
    if (fd < 0) return nullptr;

    bool ok = xsessiond_send(fd, Opcode::Authenticate, 1, user_s, pass_s);
    std::fill(pass_s.begin(), pass_s.end(), '\0'); pass_s.clear();
    if (!ok || xsessiond_recv(fd, Reply::AuthResult) != 1) { ::close(fd); return nullptr; }

    // StartSession, carrying the session-leader pid for utmpx accounting.
    uint32_t pid = static_cast<uint32_t>(leader);
    std::string leader_field(reinterpret_cast<const char*>(&pid), sizeof(pid));
    if (!xsessiond_send(fd, Opcode::StartSession, 2, leader_field, {}) ||
        xsessiond_recv(fd, Reply::SessionResult) != 1) {
        ::close(fd);
        return nullptr;
    }
    // Hold the connection open — closing it logs the session out.
    return std::unique_ptr<XsessiondSession>(new XsessiondSession(fd));
}

void XsessiondSession::end() noexcept {
    if (fd_ < 0) return;
    (void)xsessiond_send(fd_, Opcode::EndSession, 3, {}, {});
    (void)xsessiond_recv(fd_, Reply::SessionResult);
    ::close(fd_);
    fd_ = -1;
}

XsessiondSession::~XsessiondSession() { end(); }

// ── Privilege broker (phase 3) ──────────────────────────────────────────────────

int runPrivilegedOp(std::string_view verb, std::string_view arg) noexcept {
    if (verb.empty() || verb.size() > kMaxField || arg.size() > kMaxField)
        return kBrokerUnreachable;   // malformed → treat as unavailable; caller falls back

    int fd = xsessiond_connect();
    if (fd < 0) return kBrokerUnreachable;   // daemon not running
    struct Closer { int fd; ~Closer() { ::close(fd); } } closer{fd};

    if (!xsessiond_send(fd, Opcode::RunPrivilegedOp, 1,
                        std::string(verb), std::string(arg)))
        return kBrokerUnreachable;
    int32_t res = xsessiond_recv(fd, Reply::PrivilegedResult);
    if (res == kReplyError) return kBrokerUnreachable;
    return res;   // 0 ok, or a negative errno from the daemon
}

} // namespace fbx::xserver
