#pragma once
#include "shared/protocol.h"
#include <vector>
#include <mutex>

namespace fbx::xserver {

// Queues virtual key inputs from the on-screen keyboard for injection into
// the focused surface. Drain pattern matches RemoteDesktopService.
class VirtualKeyboardService {
public:
    // Called by xbus_dispatcher when InjectKey is received
    static void queueKey(const shared::VirtualKeyInput& key) noexcept;

    // Called from main event loop; returns queued inputs and clears the queue
    static std::vector<shared::InputMessage> drainInjectedKeys() noexcept;

private:
    static std::mutex                            mutex_;
    static std::vector<shared::VirtualKeyInput>  queue_;
};

} // namespace fbx::xserver
