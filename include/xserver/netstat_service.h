#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.Netstat — active-connection viewer with a kill action.
//   ListConnections → paged NetstatPage (app / remote / proto / state / pid)
//   KillConnection(id) → ack; best-effort SIGTERM to the owning pid (unprivileged
//     → own processes only), resolved by re-scanning sockstat for the id.
class NetstatService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
