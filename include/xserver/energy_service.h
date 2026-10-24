#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.Energy — per-app energy usage (ListUsage → paged EnergyPage,
// ranked by CPU% as the energy proxy; wakeups/sec is 0, unavailable on FreeBSD).
// See energy.h. Read-only.
class EnergyService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
