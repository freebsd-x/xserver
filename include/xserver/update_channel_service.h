#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
namespace fbx::xserver {
// com.freebsdx.UpdateChannel — persisted update channel (Stable/Beta). Pending
// update discovery is a follow-up (would query pkg); pending_count reports 0.
class UpdateChannelService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};
}  // namespace fbx::xserver
