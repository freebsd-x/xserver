#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.WakeLock — persisted toggle (GetState / Set* → persist + broadcast).
class WakeLockService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    // True while a keep-awake is in force (respecting a timed lock's expiry).
    // Polled by the screensaver/lock idle timers to inhibit sleep/blank/lock.
    [[nodiscard]] static bool isActive() noexcept;
};

}  // namespace fbx::xserver
