#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.Process — top processes by CPU (ListTop → paged ProcessPage) and
// Kill (SIGTERM/SIGKILL, uid-gated by the OS). Enumerated via sysctl KERN_PROC.
class ProcessService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
