#pragma once

// xsessiond wire protocol (phase 1: authentication only).
//
// One request = one AF_UNIX SOCK_SEQPACKET datagram; one reply = one datagram.
// SEQPACKET preserves message boundaries — but ONLY if the sender delimits each
// record with MSG_EOR. On FreeBSD, sends without MSG_EOR coalesce into one
// recvmsg and the boundary guarantee is lost (the same trap that bit the XBus
// transport). Every send here therefore passes MSG_NOSIGNAL | MSG_EOR, and each
// message stays ≤ kMaxMessage so a record never spans reads. Every datagram
// begins with a fixed header; variable-length fields (username, secret) follow
// the request header in order. All integers are host byte order — local-only.
//
// Kept deliberately tiny and dependency-free so both the standalone privileged
// daemon (xsessiond.cc) and the unprivileged compositor client (xsessiond_client.cc)
// can share it. Device brokering / session opcodes (phases 2–3) will extend the
// Opcode/Reply enums; the header layout is versioned so old clients fail closed.

#include <cstdint>

namespace fbx::xserver::xsessiond {

inline constexpr uint32_t kMagic       = 0x46534431u; // "FSD1"
inline constexpr uint16_t kVersion     = 1;
inline constexpr uint32_t kMaxMessage  = 4096;        // hard cap per datagram
inline constexpr uint32_t kMaxField    = 1024;        // per variable-length field
inline constexpr const char* kDefaultSocketPath = "/var/run/xsessiond.sock";
// Override for tests / running the daemon unprivileged in a scratch dir.
inline constexpr const char* kSocketPathEnv = "XSESSIOND_SOCK";

enum class Opcode : uint16_t {
    Hello        = 1,   // no fields                  → Welcome
    Authenticate = 2,   // field1=user, field2=secret → AuthResult
    // Session lifecycle. A connection carries one session: Authenticate must
    // succeed first (it sets the connection's authenticated user), then
    // StartSession opens the PAM session + utmpx login record. The session ends
    // when the client sends EndSession *or* simply closes the connection — so a
    // crashed compositor is still logged out. field1 = 4-byte session-leader pid
    // (host order), recorded in utmpx.
    StartSession = 3,   // field1=leader_pid          → SessionResult
    EndSession   = 4,   // no fields                  → SessionResult
    // Device brokering (phase 2a). The root daemon open()s an allowlisted seat
    // device node (DRM/evdev) and hands the fd to the unprivileged compositor
    // over SCM_RIGHTS, so the compositor never opens a privileged node itself.
    // Devices are leased per connection: they are closed on CloseDevice or when
    // the connection drops. field1 = device path (OpenDevice) or 4-byte device
    // id (CloseDevice), host order.
    OpenDevice   = 5,   // field1=path                → Device (+ fd via SCM_RIGHTS)
    CloseDevice  = 6,   // field1=device_id           → DeviceClosed
    // Seat activation (phase 2b). When xsessiond runs as a libseat client of
    // real seatd it relays VT-switch activation to the compositor as unsolicited
    // SeatDisable/SeatEnable push replies (below). On SeatDisable the compositor
    // must quiesce the GPU (stop page-flipping) and answer DisableAck; only then
    // does the daemon ack the disable down to seatd (drops DRM master). No
    // fields; the connection holds exactly one seat so no id is needed.
    DisableAck   = 7,   // no fields                  → (no reply; unblocks daemon)
    // Privilege broker (phase 3). The unprivileged compositor asks the root
    // daemon to perform an allowlisted privileged operation (see priv_ops.h):
    // field1 = verb, field2 = argument. The daemon validates via
    // privops::validateOp and performs ONLY known verbs with safe args — never
    // an arbitrary command. → PrivilegedResult.
    RunPrivilegedOp = 8, // field1=verb, field2=arg    → PrivilegedResult
};

enum class Reply : uint16_t {
    Welcome       = 1,  // result = capability bitmask
    AuthResult    = 2,  // result = 1 accept, 0 reject
    SessionResult = 3,  // result = 1 ok, 0 failed
    // Device reply: result >= 0 is the device id and an fd rides in the
    // ancillary SCM_RIGHTS buffer; result < 0 is a negative errno and no fd.
    Device        = 4,  // result = device id (>=0) + fd, or negative errno
    DeviceClosed  = 5,  // result = 1 closed, 0 unknown id
    // Unsolicited seat-activation push events (phase 2b). Sent by the daemon on a
    // VT switch, not in response to any request, with serial = 0. SeatDisable:
    // the seat is being taken away — the compositor must stop touching the GPU
    // and reply DisableAck. SeatEnable: the seat is back — reacquire + repaint.
    SeatDisable   = 6,  // serial = 0, result = 0 (push)
    SeatEnable    = 7,  // serial = 0, result = 0 (push)
    // Privilege broker: result = 0 on success, or a negative errno-ish code.
    PrivilegedResult = 8,
    Error         = 0xFFFF, // result = negative errno-ish code
};

// serial value stamped on unsolicited push replies (SeatDisable/SeatEnable),
// which do not correspond to any client request.
inline constexpr uint32_t kPushSerial = 0;

// Capability bits reported in Welcome.result.
inline constexpr int32_t kCapAuthenticate = 1 << 0;
inline constexpr int32_t kCapSession      = 1 << 1;
inline constexpr int32_t kCapDevice       = 1 << 2;
inline constexpr int32_t kCapActivation   = 1 << 3;  // relays SeatDisable/Enable
inline constexpr int32_t kCapPrivileged   = 1 << 4;  // runs allowlisted root ops

// Request datagram: RequestHeader, then field1_len bytes, then field2_len bytes.
struct RequestHeader {
    uint32_t magic;       // kMagic
    uint16_t version;     // kVersion
    uint16_t opcode;      // Opcode
    uint32_t serial;      // echoed back in the reply
    uint32_t field1_len;  // e.g. username length
    uint32_t field2_len;  // e.g. secret length
};

// Reply datagram: exactly this header (no trailing bytes in phase 1).
struct ReplyHeader {
    uint32_t magic;       // kMagic
    uint16_t version;     // kVersion
    uint16_t reply;       // Reply
    uint32_t serial;      // echoes the request serial
    int32_t  result;      // see per-Reply meaning above
};

} // namespace fbx::xserver::xsessiond
