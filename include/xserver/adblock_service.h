#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.AdBlock — DNS ad-blocker status/toggle. Config-backed: GetStatus
// reports persisted enabled/domain_count/list_date; SetEnabled + UpdateList
// persist and broadcast StatusChanged (neither sends a direct reply per the
// contract). Real sinkholing (hosts/unbound + blocklist fetch) needs root +
// network → follow-up.
class AdBlockService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
