#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <functional>
namespace fbx::xserver {
// com.freebsdx.DisplayScale — UI scale (100/150/200/300 %), persisted and applied
// to the live output via the hook installed by init() (which recomputes logical
// dims and notifies clients to re-layout).
class DisplayScaleService {
public:
    // Install the apply hook (wired to DisplayService::applyOutputScale in main;
    // arg is scale percent) and push the persisted value immediately. nullptr detaches.
    static void init(std::function<void(uint32_t)> apply) noexcept;

    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};
}  // namespace fbx::xserver
