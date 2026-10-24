#include "xserver/notification_service.h"
#include "xserver/xbus_dispatcher.h"
#include "xserver/ipc_socket.h"
#include <cstring>
#include <iostream>
#include <algorithm>

namespace fbx::xserver {

std::vector<Notification> NotificationService::active_notifications_;
std::set<std::string>     NotificationService::muted_apps_;
uint32_t NotificationService::next_id_ = 1;

void NotificationService::broadcastMutedApps() noexcept {
    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.Notification",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::NotificationOpcode::MutedAppsChanged);
    shared::NotifMutedAppList list{};
    list.count_ = 0;
    for (const auto& name : muted_apps_) {
        if (list.count_ >= 8) break;
        std::strncpy(list.names_[list.count_], name.c_str(),
                     sizeof(list.names_[0]) - 1);
        ++list.count_;
    }
    std::memcpy(bcast.payload_, &list, sizeof(list));
    BroadcastXBusMessage(bcast);
}

bool NotificationService::ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.Notification") != 0) return false;

    using Op = shared::NotificationOpcode;

    if (msg.opcode_ == static_cast<uint32_t>(Op::Post)) {
        shared::NotificationPayload payload{};
        std::memcpy(&payload, msg.payload_, sizeof(payload));

        // Drop silently if app is muted
        const std::string app_name(payload.app_name_);
        if (!app_name.empty() && muted_apps_.count(app_name)) {
            std::cout << "[NotificationService] Suppressed (muted app=" << app_name << ")\n";
            shared::XBusMessage response = msg;
            uint32_t zero = 0;
            std::memcpy(response.payload_, &zero, sizeof(zero));
            (void)client->sendMessage(
                {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
            return true;
        }

        // Assign a server-side ID
        payload.id_ = next_id_++;

        uint32_t timeout_ms = payload.timeout_ms_ ? payload.timeout_ms_ : 5000u;
        auto expires = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        active_notifications_.push_back({
            payload.id_,
            std::string(payload.title_),
            std::string(payload.message_),
            app_name,
            expires
        });

        std::cout << "[NotificationService] Posted #" << payload.id_
                  << ": " << payload.title_ << " — " << payload.message_ << "\n";

        // Push to the shell (and any other registered broadcast client)
        BroadcastNotification(payload);

        // Return the assigned ID so the sender can dismiss later
        shared::XBusMessage response = msg;
        std::memcpy(response.payload_, &payload.id_, sizeof(payload.id_));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        return true;
    }

    if (msg.opcode_ == static_cast<uint32_t>(Op::Dismiss)) {
        uint32_t id = 0;
        std::memcpy(&id, msg.payload_, sizeof(id));

        auto it = std::find_if(active_notifications_.begin(), active_notifications_.end(),
            [id](const Notification& n) { return n.id == id; });
        if (it != active_notifications_.end()) {
            active_notifications_.erase(it);
            BroadcastNotificationDismiss(id);
            std::cout << "[NotificationService] Dismissed #" << id << "\n";
        }
        return true;
    }

    if (msg.opcode_ == static_cast<uint32_t>(Op::MuteApp)) {
        shared::NotifAppMuteRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.app_name_[sizeof(req.app_name_) - 1] = '\0';
        if (req.app_name_[0]) {
            muted_apps_.insert(std::string(req.app_name_, strnlen(req.app_name_, sizeof(req.app_name_))));
            std::cout << "[NotificationService] Muted app: " << req.app_name_ << "\n";
            broadcastMutedApps();
        }
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&msg), sizeof(msg)});
        return true;
    }

    if (msg.opcode_ == static_cast<uint32_t>(Op::UnmuteApp)) {
        shared::NotifAppMuteRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.app_name_[sizeof(req.app_name_) - 1] = '\0';
        if (req.app_name_[0]) {
            muted_apps_.erase(std::string(req.app_name_, strnlen(req.app_name_, sizeof(req.app_name_))));
            std::cout << "[NotificationService] Unmuted app: " << req.app_name_ << "\n";
            broadcastMutedApps();
        }
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&msg), sizeof(msg)});
        return true;
    }

    if (msg.opcode_ == static_cast<uint32_t>(Op::GetMutedApps)) {
        shared::NotifMutedAppList list{};
        list.count_ = 0;
        for (const auto& name : muted_apps_) {
            if (list.count_ >= 8) break;
            std::strncpy(list.names_[list.count_], name.c_str(),
                         sizeof(list.names_[0]) - 1);
            ++list.count_;
        }
        shared::XBusMessage response = msg;
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &list, sizeof(list));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        return true;
    }

    return false;
}

const std::vector<Notification>& NotificationService::getActiveNotifications() noexcept {
    return active_notifications_;
}

void NotificationService::cleanup() noexcept {
    auto now = std::chrono::steady_clock::now();
    for (const auto& n : active_notifications_) {
        if (now > n.expires) {
            BroadcastNotificationDismiss(n.id);
        }
    }
    active_notifications_.erase(
        std::remove_if(active_notifications_.begin(), active_notifications_.end(),
                       [&now](const Notification& n) { return now > n.expires; }),
        active_notifications_.end()
    );
}

} // namespace fbx::xserver
