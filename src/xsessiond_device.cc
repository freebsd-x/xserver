// Client side of xsessiond device brokering (phase 2a). A XsessiondDeviceManager
// asks the root daemon to open an allowlisted DRM/evdev node and receives the
// resulting fd over SCM_RIGHTS, satisfying the same ISeatManager interface the
// compositor already uses for the libseat-backed SeatManager. See
// docs/xsessiond-design.md §4a/§10.2 and xserver/xsessiond_protocol.h.

#include "xserver/xsessiond_device.h"
#include "xserver/xsessiond_protocol.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace fbx::xserver {

using namespace fbx::xserver::xsessiond;

namespace {

std::string device_socket_path() {
    if (const char* env = ::getenv(kSocketPathEnv); env && env[0]) return env;
    return kDefaultSocketPath;
}

int device_connect() noexcept {
    const std::string path = device_socket_path();
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

// Send a request datagram (header + one variable field).
bool send_request(int fd, Opcode op, uint32_t serial, const std::string& f1) noexcept {
    RequestHeader hdr{};
    hdr.magic      = kMagic;
    hdr.version    = kVersion;
    hdr.opcode     = static_cast<uint16_t>(op);
    hdr.serial     = serial;
    hdr.field1_len = static_cast<uint32_t>(f1.size());
    hdr.field2_len = 0;

    std::string msg;
    msg.reserve(sizeof(hdr) + f1.size());
    msg.append(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    msg.append(f1);
    // MSG_EOR delimits the SEQPACKET record; without it FreeBSD coalesces
    // back-to-back sends into one recvmsg (see IpcSocket::sendMessage).
    return ::send(fd, msg.data(), msg.size(), MSG_NOSIGNAL | MSG_EOR)
           == static_cast<ssize_t>(msg.size());
}

// Receive a plain reply datagram (no ancillary fd). Returns result or a
// negative sentinel on protocol error.
constexpr int32_t kReplyError = -1000;
int32_t recv_reply(int fd, Reply expect) noexcept {
    ReplyHeader rep{};
    ssize_t r = ::recv(fd, &rep, sizeof(rep), 0);
    if (r != static_cast<ssize_t>(sizeof(rep))) return kReplyError;
    if (rep.magic != kMagic || rep.version != kVersion) return kReplyError;
    if (rep.reply != static_cast<uint16_t>(expect)) return kReplyError;
    return rep.result;
}

// Receive a Device reply, extracting the SCM_RIGHTS fd when result >= 0.
// Returns the device id (>=0, *passed_fd set) or a negative errno (no fd).
int32_t recv_device(int fd, int& passed_fd) noexcept {
    passed_fd = -1;
    ReplyHeader rep{};
    struct iovec io{ &rep, sizeof(rep) };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } u{};
    struct msghdr m{};
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = u.buf;
    m.msg_controllen = sizeof(u.buf);

    ssize_t r = ::recvmsg(fd, &m, MSG_CMSG_CLOEXEC);
    if (r != static_cast<ssize_t>(sizeof(rep))) return -EIO;
    if (rep.magic != kMagic || rep.version != kVersion ||
        rep.reply != static_cast<uint16_t>(Reply::Device)) {
        return -EPROTO;
    }
    if (rep.result >= 0) {
        struct cmsghdr* c = CMSG_FIRSTHDR(&m);
        if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
            c->cmsg_len == CMSG_LEN(sizeof(int))) {
            std::memcpy(&passed_fd, CMSG_DATA(c), sizeof(int));
        } else {
            return -EPROTO;   // daemon promised an fd but sent none
        }
    }
    return rep.result;
}

} // namespace

std::expected<std::unique_ptr<XsessiondDeviceManager>, std::error_code>
XsessiondDeviceManager::create() noexcept {
    int fd = device_connect();
    if (fd < 0) {
        return std::unexpected(std::make_error_code(std::errc::connection_refused));
    }
    // Confirm the daemon actually brokers devices before we commit to it.
    if (!send_request(fd, Opcode::Hello, 1, {})) {
        ::close(fd);
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    int32_t caps = recv_reply(fd, Reply::Welcome);
    if (caps == kReplyError || !(caps & kCapDevice)) {
        ::close(fd);
        return std::unexpected(std::make_error_code(std::errc::not_supported));
    }
    return std::unique_ptr<XsessiondDeviceManager>(new XsessiondDeviceManager(fd));
}

std::unique_ptr<XsessiondDeviceManager> XsessiondDeviceManager::adopt(int fd) noexcept {
    return std::unique_ptr<XsessiondDeviceManager>(new XsessiondDeviceManager(fd));
}

XsessiondDeviceManager::~XsessiondDeviceManager() noexcept {
    // Closing the connection tells the daemon to release every leased device.
    if (fd_ >= 0) ::close(fd_);
}

std::expected<SeatDevice, std::error_code>
XsessiondDeviceManager::openDevice(const std::string& path) noexcept {
    if (path.empty() || path.size() > kMaxField) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    if (!send_request(fd_, Opcode::OpenDevice, 2, path)) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    int devfd = -1;
    int32_t res = recv_device(fd_, devfd);
    if (res < 0) {
        return std::unexpected(std::error_code(-res, std::generic_category()));
    }
    return SeatDevice{ .fd_ = UniqueFd(devfd), .device_id_ = res };
}

std::expected<void, std::error_code>
XsessiondDeviceManager::closeDevice(int device_id) noexcept {
    uint32_t v = static_cast<uint32_t>(device_id);
    std::string id_field(reinterpret_cast<const char*>(&v), sizeof(v));
    if (!send_request(fd_, Opcode::CloseDevice, 3, id_field)) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    int32_t res = recv_reply(fd_, Reply::DeviceClosed);
    if (res != 1) {
        return std::unexpected(std::make_error_code(std::errc::no_such_device));
    }
    return {};
}

int XsessiondDeviceManager::getFd() const noexcept {
    return fd_;
}

std::expected<void, std::error_code>
XsessiondDeviceManager::dispatch(int /*timeout_ms*/) noexcept {
    // Drain every pending datagram without blocking. This is called from the
    // compositor's event loop when fd_ is readable, so the interesting traffic
    // here is the daemon's unsolicited SeatDisable/SeatEnable push events
    // (serial == kPushSerial). Synchronous request replies (Device/DeviceClosed)
    // are consumed inline by openDevice()/closeDevice() and shouldn't surface
    // here; if one does (races aside), it is ignored rather than mishandled.
    for (;;) {
        ReplyHeader rep{};
        ssize_t r = ::recv(fd_, &rep, sizeof(rep), MSG_DONTWAIT);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return {};   // drained
            if (errno == EINTR) continue;
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        if (r == 0) {
            // Daemon closed the connection — the seat broker is gone.
            return std::unexpected(std::make_error_code(std::errc::connection_reset));
        }
        if (r != static_cast<ssize_t>(sizeof(rep)) ||
            rep.magic != kMagic || rep.version != kVersion) {
            continue;   // short/garbled datagram — skip it
        }
        switch (static_cast<Reply>(rep.reply)) {
        case Reply::SeatDisable:
            // Quiesce the GPU first, *then* tell the daemon it may hand DRM
            // master back to seatd. Order is the whole point (design §6).
            if (on_disable_) on_disable_();
            send_request(fd_, Opcode::DisableAck, kPushSerial, {});
            break;
        case Reply::SeatEnable:
            if (on_enable_) on_enable_();
            break;
        default:
            break;   // ignore stray synchronous replies
        }
    }
}

} // namespace fbx::xserver
