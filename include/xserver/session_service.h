#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <functional>
#include <memory>

namespace fbx::xserver {

// com.freebsdx.Sessions — enumerate active login sessions (utmpx) and end them.
//   ListSessions(page) → SessionPage
//   KillSession(id)    → ack; the local graphical seat triggers logout (tear
//                        down the desktop + return to the greeter); other
//                        sessions get a best-effort SIGHUP to their leader.
//   SessionChanged     → broadcast after a session ends, so panels re-poll.
class SessionService {
public:
    // logout is invoked when KillSession targets the local graphical seat.
    static void init(std::function<void()> logout) noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

private:
    static void listSessions(const shared::XBusMessage& msg, IpcSocket* client) noexcept;
    static void killSession(const shared::XBusMessage& msg, IpcSocket* client) noexcept;

    static std::function<void()> logout_cb_;
};

}  // namespace fbx::xserver
