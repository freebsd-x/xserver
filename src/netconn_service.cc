#include "xserver/netconn_service.h"
#include "xserver/net_conn.h"
#include "xserver/async_shell.h"
#include "shared/logger.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fbx::xserver {

namespace {
// Run sockstat and return its stdout. Empty on failure (service degrades to an
// empty list rather than erroring the caller). posix_spawn, not popen: popen()
// forks in this multithreaded compositor and the child deadlocks on a libc stdio
// lock held by another thread at fork time (see async_shell.h).
std::string runSockstat() {
    return spawnCapture("sockstat -46sn 2>/dev/null");
}
}  // namespace

void NetConnService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    if (static_cast<shared::NetConnOpcode>(msg.opcode_) != shared::NetConnOpcode::ListConns)
        return;

    uint32_t page = 0;
    std::memcpy(&page, msg.payload_, sizeof(page));

    std::vector<net::Conn> conns = net::parseSockstat(runSockstat());

    shared::ConnPage pg{};
    pg.page_  = page;
    pg.total_ = static_cast<uint32_t>(conns.size());
    uint32_t start = page * 3;
    uint32_t n = 0;
    for (; n < 3 && start + n < conns.size(); ++n) {
        const net::Conn& c = conns[start + n];
        shared::ConnEntry& e = pg.entries_[n];
        std::strncpy(e.proto_,  c.proto.c_str(),  sizeof(e.proto_)  - 1);
        std::strncpy(e.local_,  c.local.c_str(),  sizeof(e.local_)  - 1);
        std::strncpy(e.remote_, c.remote.c_str(), sizeof(e.remote_) - 1);
        std::strncpy(e.state_,  net::abbrevState(c.state).c_str(), sizeof(e.state_) - 1);
        e.pid_ = c.pid;
    }
    pg.count_    = n;
    pg.has_more_ = (start + n < conns.size()) ? 1u : 0u;

    shared::XBusMessage reply{};
    reply.transaction_id_ = msg.transaction_id_;
    std::strncpy(reply.service_name_, "com.freebsdx.NetConns",
                 sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(shared::NetConnOpcode::ListConns);
    std::memcpy(reply.payload_, &pg, sizeof(pg));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

}  // namespace fbx::xserver
