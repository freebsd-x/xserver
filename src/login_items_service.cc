#include "xserver/login_items_service.h"
#include "xserver/desktop_services.h"
#include "xserver/ipc_socket.h"
#include "xserver/user_files.h"
#include "xserver/xbus_reply.h"
#include "shared/autostart.h"
#include "shared/logger.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>

#include <unistd.h>

namespace fbx::xserver {

namespace {

using namespace svc;

std::string entryPath(const std::string& id) {
    return shared::autostart::dirPath(userfiles::homeDir()) + "/" + shared::autostart::appEntryFile(id);
}

bool opensAtLogin(const std::string& id) {
    auto text = userfiles::readFile(entryPath(id));
    shared::autostart::Entry e;
    return text && shared::autostart::parse(*text, e) && shared::autostart::enabled(e);
}

}  // namespace

bool LoginItemsService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                       const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.LoginItems") != 0) return false;
    using O = shared::LoginItemOpcode;
    constexpr const char* kName = "com.freebsdx.LoginItems";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListItems: {
        auto apps = installedApps();
        uint32_t page = readPage(msg);
        shared::LoginItemPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(apps.size());
        if (page < apps.size()) {
            pg.valid_    = 1;
            pg.has_more_ = page + 1 < apps.size() ? 1u : 0u;
            std::strncpy(pg.app_name_, apps[page].name.c_str(), sizeof(pg.app_name_) - 1);
            pg.enabled_  = opensAtLogin(apps[page].id) ? 1u : 0u;
        }
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::SetItem: {
        shared::LoginItemRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.app_name_[sizeof(req.app_name_) - 1] = '\0';
        bool ok = false;
        for (const auto& a : installedApps()) {
            if (a.name != req.app_name_) continue;
            std::string path = entryPath(a.id);
            if (req.enabled_) {
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
                ok = userfiles::writeFileAtomic(path, shared::autostart::appEntry(a.name, a.exec, a.id), 0644);
            } else {
                ok = ::unlink(path.c_str()) == 0 || errno == ENOENT;
            }
            LOG_INFO("LoginItemsService: {} {} at login", a.name, req.enabled_ ? "opens" : "doesn't open");
            break;
        }
        if (!ok) LOG_WARN("LoginItemsService: can't change '{}'", req.app_name_);
        ack(msg, client, kName);
        if (ok) {
            broadcast<char>(kName, static_cast<uint32_t>(O::ItemsChanged), nullptr);
            broadcast<char>("com.freebsdx.StartupItems",
                            static_cast<uint32_t>(shared::StartupOpcode::ItemsChanged), nullptr);
        }
        break;
    }
    default: break;
    }
    return true;
}

} // namespace fbx::xserver
