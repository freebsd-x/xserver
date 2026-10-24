#include "xserver/login_items_service.h"
#include "xserver/ipc_socket.h"
#include <cstring>
#include <iostream>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

std::mutex                              LoginItemsService::mutex_;
std::vector<LoginItemsService::Item>    LoginItemsService::items_;
bool                                    LoginItemsService::loaded_ = false;

void LoginItemsService::load() noexcept {
    if (loaded_) return;
    loaded_ = true;
    // Seed a plausible set of login items. A real implementation would read the
    // user's autostart config.
    items_ = {
        {"Music",      1},
        {"Contacts",   0},
        {"Weather",    1},
        {"TaskManager",0},
        {"Notes",      1},
    };
}

void LoginItemsService::broadcastChanged() noexcept {
    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.LoginItems",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::LoginItemOpcode::ItemsChanged);
    // No payload — the shell re-fetches the list.
    BroadcastXBusMessage(bcast);
}

bool LoginItemsService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                       const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.LoginItems") != 0) return false;

    using Op = shared::LoginItemOpcode;
    std::lock_guard<std::mutex> lk(mutex_);
    load();

    shared::XBusMessage response = msg;

    switch (static_cast<Op>(msg.opcode_)) {

    case Op::ListItems: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));
        const uint32_t total = static_cast<uint32_t>(items_.size());

        shared::LoginItemPage pg{};
        pg.page_  = page;
        pg.total_ = total;
        if (page < total) {
            pg.valid_    = 1;
            pg.has_more_ = (page + 1 < total) ? 1u : 0u;
            std::strncpy(pg.app_name_, items_[page].name.c_str(),
                         sizeof(pg.app_name_) - 1);
            pg.enabled_  = items_[page].enabled;
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

    case Op::SetItem: {
        shared::LoginItemRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.app_name_[sizeof(req.app_name_) - 1] = '\0';
        bool found = false;
        for (auto& it : items_) {
            if (it.name == req.app_name_) {
                it.enabled = req.enabled_ ? 1u : 0u;
                found = true;
                break;
            }
        }
        std::cout << "[LoginItems] SetItem '" << req.app_name_ << "' -> "
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
