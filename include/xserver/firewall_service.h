#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver{

// com.freebsdx.Firewall — pf(4) control. GetStatus reads pfctl (needs root →
// reports disabled/0 unprivileged); SetEnabled enables/disables pf via the
// privilege broker (firewall.enable verb → fixed pfctl -e/-d), falling back to a
// direct best-effort pfctl; BlockAllInbound records the emergency flag (the
// actual block-all ruleset load is a follow-up). Both broadcast StatusChanged.
class FirewallService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
