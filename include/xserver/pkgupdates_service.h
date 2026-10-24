#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.PkgUpdates — the Settings "Software Updates" view of available
// package upgrades (ListUpdates → paged PkgUpdatePage) from `pkg upgrade -n`.
// Install*/CheckNow need root/network → logged for now.
class PkgUpdatesService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
