#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <functional>

namespace fbx::xserver {

// com.freebsdx.Pointer — pointer/trackpad preferences (speed, natural-scroll,
// acceleration, tap-to-click). Persisted + broadcast, and applied live to libinput
// via the hook installed by init(). (Natural scroll is applied via ScrollDir.)
class PointerService {
public:
    // Install the apply hook (wired to InputManager::setPointerConfig in main) and
    // push the persisted config to it immediately. args: speed(1-10), acceleration,
    // tap_to_click. Pass nullptr to detach.
    static void init(std::function<void(uint32_t, bool, bool)> apply) noexcept;

    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
