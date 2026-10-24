#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.WakeOnLan — send magic packets + manage saved targets.
//   Send(mac)  → ack  (UDP broadcast to 255.255.255.255:9 — works unprivileged)
//   ListTargets/AddTarget/RemoveTarget → ack, persisted to ~/.freebsdx-wol
//     (one target per page, name<TAB>mac lines)
class WakeOnLanService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
