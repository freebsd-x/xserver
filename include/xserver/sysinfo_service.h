#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.SysInfo — read-only host facts (GetInfo → SysInfoState): hostname,
// OS version, CPU model/cores, RAM, uptime, from sysctl + gethostname.
class SysInfoService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
