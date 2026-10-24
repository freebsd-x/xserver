#include "xserver/keyboard_shortcuts_service.h"
#include "xserver/ipc_socket.h"
#include <cstring>
#include <iostream>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

std::mutex                                      KeyboardShortcutsService::mutex_;
std::vector<KeyboardShortcutsService::Shortcut> KeyboardShortcutsService::shortcuts_;
bool                                            KeyboardShortcutsService::loaded_ = false;

void KeyboardShortcutsService::load() noexcept {
    if (loaded_) return;
    loaded_ = true;
    // Seed a plausible set of system shortcuts. A real implementation would read
    // the keymap/config. binding_ is display-only; only enabled_ is toggleable.
    shortcuts_ = {
        {"Screenshot",      "Cmd+Shift+4", 1},
        {"Lock Screen",     "Cmd+Ctrl+Q",  1},
        {"Mission Control", "Ctrl+Up",     1},
        {"Spotlight",       "Cmd+Space",   1},
        {"Switch App",      "Cmd+Tab",     1},
        {"Emoji Picker",    "Cmd+Ctrl+Sp", 0},
    };
}

void KeyboardShortcutsService::broadcastChanged() noexcept {
    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.KeyboardShortcuts",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::KeyboardShortcutOpcode::ShortcutsChanged);
    // No payload — the shell re-fetches the list.
    BroadcastXBusMessage(bcast);
}

bool KeyboardShortcutsService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                              const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.KeyboardShortcuts") != 0) return false;

    using Op = shared::KeyboardShortcutOpcode;
    std::lock_guard<std::mutex> lk(mutex_);
    load();

    shared::XBusMessage response = msg;

    switch (static_cast<Op>(msg.opcode_)) {

    case Op::ListShortcuts: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));
        const uint32_t total = static_cast<uint32_t>(shortcuts_.size());

        shared::KeyShortcutPage pg{};
        pg.page_  = page;
        pg.total_ = total;
        if (page < total) {
            const Shortcut& s = shortcuts_[page];
            pg.valid_    = 1;
            pg.has_more_ = (page + 1 < total) ? 1u : 0u;
            std::strncpy(pg.action_,  s.action.c_str(),  sizeof(pg.action_)  - 1);
            std::strncpy(pg.binding_, s.binding.c_str(), sizeof(pg.binding_) - 1);
            pg.enabled_  = s.enabled;
        } else {
            pg.valid_    = 0;
            pg.has_more_ = 0;
        }
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &pg, sizeof(pg));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case Op::SetShortcut: {
        shared::KeyShortcutRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.action_[sizeof(req.action_) - 1] = '\0';
        bool found = false;
        for (auto& s : shortcuts_) {
            if (s.action == req.action_) {
                s.enabled = req.enabled_ ? 1u : 0u;
                found = true;
                break;
            }
        }
        std::cout << "[KeyboardShortcuts] SetShortcut '" << req.action_ << "' -> "
                  << (req.enabled_ ? "on" : "off")
                  << (found ? "" : " (not found)") << "\n";
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        if (found) broadcastChanged();
        break;
    }

    default:
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    return true;
}

} // namespace fbx::xserver
