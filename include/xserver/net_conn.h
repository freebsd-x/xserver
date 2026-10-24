#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Pure parsing for the connection-inspection services (com.freebsdx.NetConns and
// com.freebsdx.Netstat). Both enumerate active sockets from sockstat(1); this
// header turns its text into a neutral Conn list and normalises proto/state, so
// the logic is unit-testable without running the command.
namespace fbx::xserver::net {

struct Conn {
    std::string proto;    // normalised "TCP" / "UDP"
    std::string local;    // "addr:port"
    std::string remote;   // "addr:port", or "-" for a listening socket
    std::string state;    // full TCP state ("ESTABLISHED", "LISTEN", …) or ""
    std::string command;  // owning process name
    uint32_t    pid = 0;  // owning pid (0 = unknown)
};

// "tcp4"/"tcp6"→"TCP", "udp4"/"udp6"→"UDP"; anything else uppercased as-is.
std::string normProto(std::string_view p) noexcept;

// Abbreviate a TCP state to <=6 chars for the compact NetConns column:
// ESTABLISHED→ESTABL, TIME_WAIT→TIMEWT, CLOSE_WAIT→CLOSWT, SYN_SENT→SYNSNT, …
std::string abbrevState(std::string_view full) noexcept;

// Stable non-zero id for a connection (FNV-1a of proto|local|remote), so a
// client can ListConnections then KillConnection by id across a re-scan.
uint32_t connId(std::string_view proto, std::string_view local,
                std::string_view remote) noexcept;

// Parse `sockstat -46sn` output. Keeps only tcp/udp rows (skips UNIX sockets and
// the header). A "*:*" foreign address becomes remote "-" with state "LISTEN".
std::vector<Conn> parseSockstat(std::string_view out) noexcept;

}  // namespace fbx::xserver::net
