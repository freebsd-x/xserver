#include "xserver/default_apps_service.h"
#include "xserver/ipc_socket.h"
#include <cstring>
#include <iostream>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

std::mutex                                  DefaultAppsService::mutex_;
std::vector<DefaultAppsService::Category>   DefaultAppsService::categories_;
bool                                        DefaultAppsService::loaded_ = false;

void DefaultAppsService::load() noexcept {
    if (loaded_) return;
    loaded_ = true;
    // Seed the category → handler table. A real implementation would read the
    // MIME/association database. Each category lists its candidate handlers;
    // idx points at the current default (0).
    categories_ = {
        {"Image Viewer", {"ImageViewer.xapp", "Browser.xapp"},                    0},
        {"Music",        {"MusicPlayer.xapp", "Browser.xapp"},                    0},
        {"PDF Viewer",   {"PDFViewer.xapp", "ImageViewer.xapp", "Browser.xapp"}, 0},
        {"Text Editor",  {"Notes.xapp", "Terminal.xapp"},                         0},
        {"Web Browser",  {"Browser.xapp", "Terminal.xapp"},                       0},
    };
}

void DefaultAppsService::broadcastChanged() noexcept {
    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.DefaultApps",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::DefaultAppsOpcode::DefaultsChanged);
    // No payload — the shell re-fetches the list.
    BroadcastXBusMessage(bcast);
}

bool DefaultAppsService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                        const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.DefaultApps") != 0) return false;

    using Op = shared::DefaultAppsOpcode;
    std::lock_guard<std::mutex> lk(mutex_);
    load();

    shared::XBusMessage response = msg;

    switch (static_cast<Op>(msg.opcode_)) {

    case Op::ListDefaults: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));
        const uint32_t total = static_cast<uint32_t>(categories_.size());

        shared::DefaultAppPage pg{};
        pg.page_  = page;
        pg.total_ = total;
        if (page < total) {
            const Category& c = categories_[page];
            pg.valid_    = 1;
            pg.has_more_ = (page + 1 < total) ? 1u : 0u;
            std::strncpy(pg.category_, c.name.c_str(), sizeof(pg.category_) - 1);
            if (!c.handlers.empty())
                std::strncpy(pg.handler_, c.handlers[c.idx].c_str(),
                             sizeof(pg.handler_) - 1);
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

    case Op::CycleDefault: {
        shared::DefaultAppRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.category_[sizeof(req.category_) - 1] = '\0';
        bool found = false;
        for (auto& c : categories_) {
            if (c.name == req.category_) {
                if (c.handlers.size() > 1)
                    c.idx = (c.idx + 1) % static_cast<uint32_t>(c.handlers.size());
                found = true;
                std::cout << "[DefaultApps] Cycle '" << c.name << "' -> "
                          << (c.handlers.empty() ? "(none)" : c.handlers[c.idx]) << "\n";
                break;
            }
        }
        if (!found)
            std::cout << "[DefaultApps] Cycle '" << req.category_ << "' (not found)\n";
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
