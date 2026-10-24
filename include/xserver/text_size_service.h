#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.TextSize — persisted UI text-size preference (0=Small,1=Normal,
// 2=Large). Persists the choice and broadcasts SizeChanged so apps can rescale.
class TextSizeService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
