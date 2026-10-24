#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <functional>

namespace fbx::xserver {

// com.freebsdx.CursorSize — cursor-size preference (0=Small,1=Medium,2=Large),
// persisted and applied to the live DRM cursor via the hook installed by init().
class CursorSizeService {
public:
    // Install the apply hook (wired to the compositor cursor scale in main) and
    // push the persisted size to it immediately. Pass nullptr to detach.
    static void init(std::function<void(uint32_t)> apply) noexcept;

    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
