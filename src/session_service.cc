#include "xserver/session_service.h"
#include "xserver/session_info.h"
#include "shared/logger.h"

#include <csignal>
#include <cstring>
#include <string>
#include <vector>

#include <utmpx.h>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

std::function<void()> SessionService::logout_cb_;

void SessionService::init(std::function<void()> logout) noexcept {
    logout_cb_ = std::move(logout);
}

namespace {
// Snapshot USER_PROCESS records from utmpx into wire SessionEntry rows.
std::vector<shared::SessionEntry> enumerate() {
    std::vector<shared::SessionEntry> out;
    setutxent();
    for (struct utmpx* u = getutxent(); u; u = getutxent()) {
        if (u->ut_type != USER_PROCESS) continue;
        shared::SessionEntry e{};
        e.login_time_ = static_cast<uint64_t>(u->ut_tv.tv_sec);
        e.session_id_ = session::sessionId(u->ut_line, u->ut_user);
        const char* host = u->ut_host;
        std::strncpy(e.type_, session::classifyType(u->ut_line, host),
                     sizeof(e.type_) - 1);
        std::strncpy(e.user_, u->ut_user, sizeof(e.user_) - 1);
        // Prefer the remote host as the source; fall back to the tty line.
        std::strncpy(e.source_, (host && *host) ? host : u->ut_line,
                     sizeof(e.source_) - 1);
        out.push_back(e);
    }
    endutxent();
    return out;
}
}  // namespace

void SessionService::listSessions(const shared::XBusMessage& msg,
                                  IpcSocket* client) noexcept {
    uint32_t page = 0;
    std::memcpy(&page, msg.payload_, sizeof(page));

    shared::SessionPage pg = session::buildPage(enumerate(), page);

    shared::XBusMessage reply{};
    reply.transaction_id_ = msg.transaction_id_;   // echo for the caller's Call
    std::strncpy(reply.service_name_, "com.freebsdx.Sessions",
                 sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(shared::SessionOpcode::ListSessions);
    std::memcpy(reply.payload_, &pg, sizeof(pg));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

void SessionService::killSession(const shared::XBusMessage& msg,
                                 IpcSocket* client) noexcept {
    shared::SessionKillRequest req{};
    std::memcpy(&req, msg.payload_, sizeof(req));

    // Re-scan utmpx to resolve the id → line/pid (ids are derived, not indices).
    bool acted = false, is_gui = false;
    setutxent();
    for (struct utmpx* u = getutxent(); u; u = getutxent()) {
        if (u->ut_type != USER_PROCESS) continue;
        if (session::sessionId(u->ut_line, u->ut_user) != req.session_id_) continue;
        if (session::isGuiSeat(u->ut_line)) {
            is_gui = true;                       // handled after endutxent()
        } else if (u->ut_pid > 0) {
            // Best-effort: SIGHUP the session leader. Unprivileged, so this only
            // succeeds for the caller's own sessions — logged either way.
            if (::kill(u->ut_pid, SIGHUP) == 0)
                LOG_INFO("SessionService: SIGHUP session {} (pid {})",
                         req.session_id_, u->ut_pid);
            else
                LOG_WARN("SessionService: cannot signal session {} (pid {})",
                         req.session_id_, u->ut_pid);
        }
        acted = true;
        break;
    }
    endutxent();

    // Ack first so the caller isn't left waiting even if logout tears things down.
    shared::XBusMessage reply{};
    reply.transaction_id_ = msg.transaction_id_;
    std::strncpy(reply.service_name_, "com.freebsdx.Sessions",
                 sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(shared::SessionOpcode::KillSession);
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});

    // Broadcast the change so other panels re-poll.
    if (acted) {
        shared::XBusMessage bcast{};
        std::strncpy(bcast.service_name_, "com.freebsdx.Sessions",
                     sizeof(bcast.service_name_) - 1);
        bcast.opcode_ = static_cast<uint32_t>(shared::SessionOpcode::SessionChanged);
        BroadcastXBusMessage(bcast);
    }

    if (is_gui) {
        LOG_INFO("SessionService: logout requested for the graphical seat");
        if (logout_cb_) logout_cb_();
    }
}

void SessionService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::SessionOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListSessions: listSessions(msg, client.get()); break;
    case O::KillSession:  killSession(msg, client.get());  break;
    default: break;
    }
}

}  // namespace fbx::xserver
