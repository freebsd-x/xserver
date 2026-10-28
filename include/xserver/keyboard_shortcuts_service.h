#pragma once

#include "shared/protocol.h"
#include <memory>

namespace fbx::xserver {

class IpcSocket;

// com.freebsdx.KeyboardShortcuts — the desktop's real global shortcuts
// (shortcuts.h), one per page. SetShortcut switches one on/off: the key
// handlers check shortcuts::enabled(), so a disabled shortcut's keys go to the
// focused app. Persisted (ConfigService "shortcuts.disabled"); init() restores.
class KeyboardShortcutsService {
public:
    static void init() noexcept;
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;
};

} // namespace fbx::xserver
