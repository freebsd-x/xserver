#include "xserver/security_context_service.h"
#include <cstring>
#include <iostream>

namespace fbx::xserver {

std::unordered_map<uint32_t, SurfaceSecurityContext>
    SecurityContextService::surface_contexts_;

void SecurityContextService::ProcessMessage(
        std::shared_ptr<IpcSocket> client,
        const shared::XBusMessage& msg) noexcept {
    if (!client) return;

    const auto op = static_cast<shared::SecurityContextOpcode>(msg.opcode_);

    if (op == shared::SecurityContextOpcode::SetContext) {
        shared::SecurityContextInfo info{};
        std::memcpy(&info, msg.payload_, sizeof(info));
        info.app_id_[sizeof(info.app_id_) - 1]              = '\0';
        info.sandbox_engine_[sizeof(info.sandbox_engine_) - 1] = '\0';
        info.instance_id_[sizeof(info.instance_id_) - 1]    = '\0';

        if (info.surface_id_ == 0) {
            // Socket-level identity (native xkit app)
            client->setAppId(info.app_id_);
            client->setSandboxEngine(info.sandbox_engine_);
            client->setInstanceId(info.instance_id_);
            std::cout << "[SecurityContext] Socket identity: app_id=" << info.app_id_
                      << " engine=" << info.sandbox_engine_ << "\n";
        } else {
            // Per-surface identity (Wayland bridge client)
            surface_contexts_[info.surface_id_] = {
                info.app_id_, info.sandbox_engine_, info.instance_id_};
            std::cout << "[SecurityContext] Surface " << info.surface_id_
                      << " identity: app_id=" << info.app_id_
                      << " engine=" << info.sandbox_engine_ << "\n";
        }

        // Echo back as acknowledgement
        shared::XBusMessage resp = msg;
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
    }
    else if (op == shared::SecurityContextOpcode::GetContext) {
        shared::SecurityContextInfo info{};
        uint32_t surface_id = 0;
        std::memcpy(&surface_id, msg.payload_, sizeof(surface_id));

        if (surface_id != 0) {
            auto it = surface_contexts_.find(surface_id);
            if (it != surface_contexts_.end()) {
                std::strncpy(info.app_id_, it->second.app_id.c_str(),
                             sizeof(info.app_id_) - 1);
                std::strncpy(info.sandbox_engine_, it->second.sandbox_engine.c_str(),
                             sizeof(info.sandbox_engine_) - 1);
                std::strncpy(info.instance_id_, it->second.instance_id.c_str(),
                             sizeof(info.instance_id_) - 1);
            }
        } else {
            std::strncpy(info.app_id_, client->appId().c_str(),
                         sizeof(info.app_id_) - 1);
            std::strncpy(info.sandbox_engine_, client->sandboxEngine().c_str(),
                         sizeof(info.sandbox_engine_) - 1);
            std::strncpy(info.instance_id_, client->instanceId().c_str(),
                         sizeof(info.instance_id_) - 1);
        }

        shared::XBusMessage resp = msg;
        std::memcpy(resp.payload_, &info, sizeof(info));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
    }
}

std::string SecurityContextService::GetAppId(
        const IpcSocket& socket, uint32_t surface_id) noexcept {
    if (surface_id != 0) {
        auto it = surface_contexts_.find(surface_id);
        if (it != surface_contexts_.end() && !it->second.app_id.empty())
            return it->second.app_id;
    }
    return socket.appId();
}

void SecurityContextService::OnSurfaceClosed(uint32_t surface_id) noexcept {
    surface_contexts_.erase(surface_id);
}

} // namespace fbx::xserver
