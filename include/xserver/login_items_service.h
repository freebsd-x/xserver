#pragma once
#include "shared/protocol.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fbx::xserver {
class IpcSocket;

// com.freebsdx.LoginItems — apps launched at login; each can be toggled on/off.
// Per-page request/response: ListItems(page) returns the single app at that
// index (valid_/has_more_ drive the shell's page walk); SetItem flips the
// enabled flag for a named app and broadcasts ItemsChanged so the shell
// re-fetches. Backed by a seeded in-memory list (no real launchd yet).
class LoginItemsService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

private:
    struct Item { std::string name; uint32_t enabled; };

    static std::mutex          mutex_;
    static std::vector<Item>   items_;
    static bool                loaded_;

    static void load() noexcept;   // seed defaults once
    static void broadcastChanged() noexcept;
};

} // namespace fbx::xserver
