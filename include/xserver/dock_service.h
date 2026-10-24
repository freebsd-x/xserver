#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
namespace fbx::xserver {
// com.freebsdx.Dock — persisted dock settings (position/autohide/icon size).
class DockService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};
}  // namespace fbx::xserver
