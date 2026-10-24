#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <functional>
#include <memory>

namespace fbx::xserver {

// com.freebsdx.ScrollDir — natural vs traditional scroll. Persisted + broadcast,
// and actually applied to input devices via the callback registered in init().
class ScrollDirService {
public:
    // apply is called with the natural-scroll flag on change and once at startup
    // with the persisted value (wired to InputManager::setNaturalScroll).
    static void init(std::function<void(bool)> apply) noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
