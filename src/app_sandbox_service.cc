#include "xserver/app_sandbox_service.h"
#include "shared/logger.h"

#include <cstring>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

PermissionStore& AppSandboxService::store() noexcept {
    static PermissionStore s;
    return s;
}

void AppSandboxService::init() noexcept {
    store().load();
    LOG_INFO("AppSandboxService: ready ({} apps with overrides)", store().list().size());
}

bool AppSandboxService::isAllowed(const std::string& app,
                                  PermissionStore::Capability cap) noexcept {
    return store().isAllowed(app, cap);
}

void AppSandboxService::sendPage(IpcSocket* client, uint32_t tid, uint32_t page) noexcept {
    const auto apps = store().list();
    shared::AppSandboxPage pg{};
    pg.page_  = page;
    pg.total_ = static_cast<uint32_t>(apps.size());
    if (page < apps.size()) {
        const auto& a = apps[page];
        pg.valid_ = 1u;
        std::strncpy(pg.identifier_,   a.app.c_str(), sizeof(pg.identifier_) - 1);
        std::strncpy(pg.display_name_, a.app.c_str(), sizeof(pg.display_name_) - 1);
        pg.net_allowed_ = a.allowed[PermissionStore::Net]        ? 1u : 0u;
        pg.cam_allowed_ = a.allowed[PermissionStore::Camera]     ? 1u : 0u;
        pg.mic_allowed_ = a.allowed[PermissionStore::Microphone] ? 1u : 0u;
        pg.fs_allowed_  = a.allowed[PermissionStore::Filesystem] ? 1u : 0u;
        pg.gpu_allowed_ = a.allowed[PermissionStore::Gpu]        ? 1u : 0u;
        pg.screencap_allowed_ = a.allowed[PermissionStore::ScreenCapture] ? 1u : 0u;
    }
    pg.has_more_ = (page + 1 < apps.size()) ? 1u : 0u;

    shared::XBusMessage reply{};
    reply.transaction_id_ = tid;   // echo so the client's Call callback matches
    std::strncpy(reply.service_name_, "com.freebsdx.AppSandbox",
                 sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(shared::AppSandboxOpcode::ListApps);
    std::memcpy(reply.payload_, &pg, sizeof(pg));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

void AppSandboxService::handleMessage(const shared::XBusMessage& msg,
                                      std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::AppSandboxOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListApps: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));
        sendPage(client.get(), msg.transaction_id_, page);
        break;
    }
    case O::SetPermission: {
        shared::AppSandboxPermRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.identifier_[sizeof(req.identifier_) - 1] = '\0';
        if (req.permission_ < PermissionStore::kNumCaps) {
            store().set(req.identifier_,
                        static_cast<PermissionStore::Capability>(req.permission_),
                        req.allowed_ != 0);
            LOG_INFO("AppSandboxService: {} cap {} -> {}", req.identifier_,
                     req.permission_, req.allowed_ ? "grant" : "revoke");
            // Broadcast so the shell re-fetches (no payload per protocol).
            shared::XBusMessage bcast{};
            std::strncpy(bcast.service_name_, "com.freebsdx.AppSandbox",
                         sizeof(bcast.service_name_) - 1);
            bcast.opcode_ = static_cast<uint32_t>(O::PermsChanged);
            BroadcastXBusMessage(bcast);
        }
        break;
    }
    default:
        break;
    }
}

}  // namespace fbx::xserver
