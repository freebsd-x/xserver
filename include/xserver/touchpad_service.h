#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <functional>
namespace fbx::xserver {
// com.freebsdx.Touchpad — persisted touchpad enable/disable, applied live to the
// input devices via the hook installed by init().
class TouchpadService {
public:
    // Install the apply hook (wired to InputManager::setTouchpadEnabled in main)
    // and push the persisted value to it immediately. Pass nullptr to detach.
    static void init(std::function<void(bool)> apply) noexcept;

    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};
}  // namespace fbx::xserver
