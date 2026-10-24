#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
namespace fbx::xserver {
// com.freebsdx.Privacy — global camera/mic block flags. Persist + broadcast; the
// camera/audio services would consult these to actually deny access (enforcement
// hook not yet wired). The "currently using" app fields are reported empty (no
// live device-usage tracking yet).
class PrivacyService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
    [[nodiscard]] static bool cameraBlocked() noexcept;
    [[nodiscard]] static bool micBlocked() noexcept;
};
}  // namespace fbx::xserver
