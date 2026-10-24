#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.BatteryHealth — battery wear info + optimized-charging toggle.
// health_pct_/cycle_count_ are real, parsed from `acpiconf -i 0` (unprivileged);
// optimized_charging_ is a persisted preference (FreeBSD has no charge-limit
// control, so it has no hardware effect — persist + broadcast only).
class BatteryHealthService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
