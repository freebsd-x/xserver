#pragma once
#include "shared/protocol.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fbx::xserver {
class IpcSocket;

// com.freebsdx.KeyboardShortcuts — system shortcut bindings, each toggleable.
// Per-page request/response: ListShortcuts(page) returns the action at that
// index with its binding (display-only) and enabled flag; SetShortcut flips the
// enabled flag for a named action and broadcasts ShortcutsChanged so the shell
// re-fetches. Backed by a seeded in-memory table (no real keymap wiring yet).
class KeyboardShortcutsService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

private:
    struct Shortcut { std::string action; std::string binding; uint32_t enabled; };

    static std::mutex             mutex_;
    static std::vector<Shortcut>  shortcuts_;
    static bool                   loaded_;

    static void load() noexcept;   // seed defaults once
    static void broadcastChanged() noexcept;
};

} // namespace fbx::xserver
