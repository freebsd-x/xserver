#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.BatteryDrain — per-app energy impact (ListDrain → paged
// BatteryDrainPage). FreeBSD has no per-process power accounting, so impact is
// approximated by CPU share (see battery_drain.h). RefreshDrain acks.
class BatteryDrainService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
