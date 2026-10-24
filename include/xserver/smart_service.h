#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.Smart — SMART disk health (SmartOpcode::ListDrives → paged
// SmartPage). Drive list from `sysctl -n kern.disks` (always readable); per-drive
// health/temp/reallocated/power-on-hours from `smartctl -H -A` (needs the
// smartmontools port + root → degrades to health "N/A" otherwise).
class SmartService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
