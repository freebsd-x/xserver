#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
namespace fbx::xserver {
// com.freebsdx.FocusMode — a Do-Not-Disturb style focus session with an optional
// timer. Start/Stop persist the session; GetState reports whether it's active and
// the remaining seconds (auto-expiring a timed session).
class FocusModeService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
    [[nodiscard]] static shared::FocusModeState currentState() noexcept;
};
}  // namespace fbx::xserver
