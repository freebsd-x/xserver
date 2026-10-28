#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.DoNotDisturb — persisted toggle state (GetState / Set* → persist +
// StateChanged broadcast).
class DoNotDisturbService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    // DND on right now (a timed DND that has run out counts as off).
    [[nodiscard]] static bool isActive() noexcept;
};

}  // namespace fbx::xserver
