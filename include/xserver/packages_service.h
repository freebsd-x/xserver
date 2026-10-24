#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.Packages — available package upgrades (ListUpdates → paged
// PackagePage) from `pkg upgrade -n`. UpdateAll/HoldPackage need root/network →
// logged for now. Read path degrades to empty when pkg is unavailable.
class PackagesService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
