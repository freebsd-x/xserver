#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.ScreenTimeout — how long without input before the screen goes dark, in
// seconds (0 = never). Kept and broadcast; IdleService acts on it.
class ScreenTimeoutService {
public:
    [[nodiscard]] static uint32_t seconds() noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
