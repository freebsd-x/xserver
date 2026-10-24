#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <iostream>
#include <stddef.h>
#include <time.h>
#include <atomic>
#include <cstdlib>

namespace fbx::xserver {

// The transport is SOCK_SEQPACKET (see connect() / the compositor's master
// socket), so every recvmsg returns exactly one whole datagram — provided it
// fits the receive buffer. If a datagram is larger, SEQPACKET truncates it: the
// kernel discards the tail and sets MSG_TRUNC. The receive buffers below are
// sized to the largest message (XBusMessage for receiveIntoSlot; 4 KiB for the
// legacy receiveMessage). Enforce that invariant at compile time so growing a
// message struct past the buffer is a build error, not a silent runtime drop.
static_assert(sizeof(shared::InputMessage)  <= sizeof(shared::XBusMessage),
              "InputMessage must fit the SEQPACKET slot buffer (sizeof XBusMessage)");
static_assert(sizeof(shared::RenderMessage) <= sizeof(shared::XBusMessage),
              "RenderMessage must fit the SEQPACKET slot buffer (sizeof XBusMessage)");
static_assert(sizeof(shared::XBusMessage)   <= 4096,
              "XBusMessage must fit receiveMessage()'s 4 KiB buffer");

IpcSocket::IpcSocket(UniqueFd fd) noexcept : fd_(std::move(fd)) {}

IpcSocket::IpcSocket(const std::string& path) noexcept : socket_path_(path) {}

bool IpcSocket::connect() noexcept {
    // SOCK_SEQPACKET (not STREAM): preserve message boundaries. With SOCK_STREAM
    // the kernel coalesces back-to-back messages into a single recvmsg, and the
    // receiver — which treats each read as exactly one message — then discards the
    // tail and desyncs the byte stream (producing phantom all-zero reads and lost
    // frames/input). SEQPACKET makes each recvmsg return exactly one sent message
    // (with its SCM_RIGHTS fd), so no framing layer is needed — BUT ONLY IF the
    // sender delimits records with MSG_EOR (see sendMessage). On FreeBSD, sends
    // without MSG_EOR coalesce and SEQPACKET degrades to a byte stream. Max
    // message is sizeof(XBusMessage) == the slot buffer, so records never split;
    // an oversized one would truncate (MSG_TRUNC), guarded above at compile time.
    int s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (s < 0) return false;

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    
    std::string path = socket_path_;
    if (path.empty()) path = "/tmp/freebsd-x.sock";
    
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    
    socklen_t len = offsetof(struct sockaddr_un, sun_path) + strlen(addr.sun_path);

    if (::connect(s, (struct sockaddr*)&addr, len) < 0) {
        int err = errno;
        if (err != ECONNREFUSED) { // Don't spam during silent retries if desired
             std::cerr << "[IpcSocket] connect() to " << path << " failed: " << std::strerror(err) << "\n";
        }
        ::close(s);
        return false;
    }
    fd_ = UniqueFd(s);
    return true;
}

std::expected<void, std::error_code> IpcSocket::sendMessage(std::span<const uint8_t> data, int fd_to_send) const noexcept {
    if (!fd_.isValid()) {
        return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
    }

    struct msghdr msg;
    std::memset(&msg, 0, sizeof(msg));
    struct iovec iov[1];
    
    char dummy = '*';
    if (data.empty()) {
        iov[0].iov_base = &dummy;
        iov[0].iov_len = 1;
    } else {
        iov[0].iov_base = const_cast<uint8_t*>(data.data()); 
        iov[0].iov_len = data.size();
    }

    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } u;

    if (fd_to_send >= 0) {
        msg.msg_control = u.buf;
        msg.msg_controllen = sizeof(u.buf);

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));

        std::memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));
    }

    // Time the sendmsg(2) syscall with CLOCK_MONOTONIC.
    // Atomic counters avoid any lock on the hot path; we only log periodically.
    static std::atomic<uint64_t> g_total_ns{0};
    static std::atomic<uint64_t> g_call_count{0};

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    // MSG_EOR marks the end of a record. On FreeBSD's AF_UNIX SOCK_SEQPACKET the
    // boundary is NOT implicit per sendmsg: without MSG_EOR the kernel treats
    // back-to-back sends as one continued record, so recvmsg coalesces them and
    // the transport silently degrades to a byte stream (the very coalescing this
    // socket type was chosen to avoid). With MSG_EOR each message is its own
    // record and recvmsg returns exactly one per call.
    ssize_t sent = sendmsg(fd_.get(), &msg, MSG_EOR);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    if (sent < 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }

    uint64_t elapsed_ns = static_cast<uint64_t>(t1.tv_sec  - t0.tv_sec)  * 1'000'000'000ULL
                        + static_cast<uint64_t>(t1.tv_nsec - t0.tv_nsec);
    g_total_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
    uint64_t n = g_call_count.fetch_add(1, std::memory_order_relaxed) + 1;

    if (n % kSendLogInterval == 0) {
        double avg_us = static_cast<double>(g_total_ns.load(std::memory_order_relaxed))
                      / static_cast<double>(n) / 1000.0;
        std::cout << "[IpcSocket] sendmsg avg latency: " << avg_us
                  << " µs over " << n << " calls\n";
    }

    return {};
}

std::expected<XBusMessagePool::PooledSlot, std::error_code>
IpcSocket::receiveIntoSlot(XBusMessagePool& pool, UniqueFd& out_fd) const noexcept {
    if (!fd_.isValid())
        return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));

    auto owned = pool.acquireOwned();  // never null (thread_local fallback)
    XBusMessagePool::Slot* slot = owned.get();
    slot->byte_count_ = 0;

    char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov{ slot->buf_, sizeof(slot->buf_) };
    struct msghdr mhdr{};
    mhdr.msg_iov        = &iov;
    mhdr.msg_iovlen     = 1;
    mhdr.msg_control    = control;
    mhdr.msg_controllen = sizeof(control);

    ssize_t n = recvmsg(fd_.get(), &mhdr, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    if (n == 0)
        return std::unexpected(std::make_error_code(std::errc::connection_aborted));

    // A truncated datagram means a message exceeded the slot buffer (guarded
    // against at compile time above, so this is defence-in-depth). Log rather
    // than error: the caller treats any non-EAGAIN error as a client disconnect,
    // and dropping the connection over one oversized frame would be worse.
    if (mhdr.msg_flags & MSG_TRUNC)
        std::cerr << "[IpcSocket] WARNING: datagram truncated to " << n
                  << " bytes (exceeds slot buffer); tail discarded by kernel\n";
    if (mhdr.msg_flags & MSG_CTRUNC)
        std::cerr << "[IpcSocket] WARNING: ancillary data (SCM_RIGHTS fd) truncated on recvmsg\n";

    slot->byte_count_ = n;

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&mhdr);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int fd;
        std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
        out_fd = UniqueFd(fd);
    }

    return owned;
}

std::expected<std::pair<std::vector<uint8_t>, UniqueFd>, std::error_code> IpcSocket::receiveMessage() const noexcept {
    if (!fd_.isValid()) {
        return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
    }

    char buf[4096];
    char control[CMSG_SPACE(sizeof(int))];

    struct iovec iov[1];
    iov[0].iov_base = buf;
    iov[0].iov_len = sizeof(buf);

    struct msghdr msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ssize_t bytes_read = recvmsg(fd_.get(), &msg, 0);
    
    if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
        return std::unexpected(std::error_code(errno, std::generic_category()));
    } else if (bytes_read == 0) {
        return std::unexpected(std::make_error_code(std::errc::connection_aborted));
    }

    // Oversized datagram (see receiveIntoSlot): the tail is already discarded by
    // the kernel. Log rather than error — this caller path also treats non-EAGAIN
    // errors as a fatal disconnect. Compile-time static_asserts keep this
    // unreachable for the current message set.
    if (msg.msg_flags & MSG_TRUNC)
        std::cerr << "[IpcSocket] WARNING: datagram truncated to " << bytes_read
                  << " bytes (exceeds 4 KiB buffer); tail discarded by kernel\n";
    if (msg.msg_flags & MSG_CTRUNC)
        std::cerr << "[IpcSocket] WARNING: ancillary data (SCM_RIGHTS fd) truncated on recvmsg\n";

    std::vector<uint8_t> received_data(buf, buf + bytes_read);
    UniqueFd received_fd;

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int fd;
        std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
        received_fd = UniqueFd(fd);
    }

    return std::make_pair(received_data, std::move(received_fd));
}

} // namespace fbx::xserver
