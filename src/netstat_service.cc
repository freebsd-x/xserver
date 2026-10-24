#include "xserver/netstat_service.h"
#include "xserver/net_conn.h"
#include "xserver/async_shell.h"
#include "shared/logger.h"

#include <array>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fbx::xserver {

namespace {
std::string runSockstat() {
    // posix_spawn, not popen: popen() forks in this multithreaded compositor and the
    // child deadlocks on a libc stdio lock held by another thread (see async_shell.h).
    return spawnCapture("sockstat -46sn 2>/dev/null");
}

void sendAck(IpcSocket* client, uint32_t tid, shared::NetstatOpcode op) {
    shared::XBusMessage reply{};
    reply.transaction_id_ = tid;
    std::strncpy(reply.service_name_, "com.freebsdx.Netstat",
                 sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(op);
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

void listConnections(const shared::XBusMessage& msg, IpcSocket* client) {
    uint32_t page = 0;
    std::memcpy(&page, msg.payload_, sizeof(page));
    std::vector<net::Conn> conns = net::parseSockstat(runSockstat());

    shared::NetstatPage pg{};
    pg.page_  = page;
    pg.total_ = static_cast<uint32_t>(conns.size());
    uint32_t start = page * 2;
    uint32_t n = 0;
    for (; n < 2 && start + n < conns.size(); ++n) {
        const net::Conn& c = conns[start + n];
        shared::NetstatConnection& e = pg.conns_[n];
        std::strncpy(e.app_,    c.command.empty() ? "(unknown)" : c.command.c_str(),
                     sizeof(e.app_) - 1);
        std::strncpy(e.remote_, c.remote.c_str(), sizeof(e.remote_) - 1);
        // proto_ lowercase per the protocol's example ("tcp"/"udp").
        std::string lp = c.proto; for (char& ch : lp) ch = static_cast<char>(::tolower(ch));
        std::strncpy(e.proto_, lp.c_str(), sizeof(e.proto_) - 1);
        std::strncpy(e.state_, c.state.c_str(), sizeof(e.state_) - 1);
        e.pid_     = c.pid;
        e.conn_id_ = net::connId(c.proto, c.local, c.remote);
    }
    pg.count_    = n;
    pg.has_more_ = (start + n < conns.size()) ? 1u : 0u;

    shared::XBusMessage reply{};
    reply.transaction_id_ = msg.transaction_id_;
    std::strncpy(reply.service_name_, "com.freebsdx.Netstat",
                 sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(shared::NetstatOpcode::ListConnections);
    std::memcpy(reply.payload_, &pg, sizeof(pg));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

void killConnection(const shared::XBusMessage& msg, IpcSocket* client) {
    shared::NetstatKillRequest req{};
    std::memcpy(&req, msg.payload_, sizeof(req));

    // Resolve the id back to a live connection's pid by re-scanning.
    uint32_t pid = 0;
    for (const net::Conn& c : net::parseSockstat(runSockstat())) {
        if (net::connId(c.proto, c.local, c.remote) == req.conn_id_) { pid = c.pid; break; }
    }
    if (pid > 0) {
        // Best-effort: SIGTERM the owning process. Unprivileged, so this only
        // succeeds for the caller's own processes; logged either way. (RST via
        // the peer fd would need access to another process's descriptor.)
        if (::kill(static_cast<pid_t>(pid), SIGTERM) == 0)
            LOG_INFO("NetstatService: terminated pid {} for conn {}", pid, req.conn_id_);
        else
            LOG_WARN("NetstatService: cannot signal pid {} for conn {}", pid, req.conn_id_);
    } else {
        LOG_WARN("NetstatService: conn {} not found (already closed?)", req.conn_id_);
    }
    sendAck(client, msg.transaction_id_, shared::NetstatOpcode::KillConnection);
}
}  // namespace

void NetstatService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::NetstatOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListConnections: listConnections(msg, client.get()); break;
    case O::KillConnection:  killConnection(msg, client.get());  break;
    default: break;
    }
}

}  // namespace fbx::xserver
