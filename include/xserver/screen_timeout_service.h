#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.ScreenTimeout — persisted auto-lock/blank delay in seconds
// (0 = never). Persist + broadcast; the screensaver/lock idle timers can consult
// the stored value.
class ScreenTimeoutService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
