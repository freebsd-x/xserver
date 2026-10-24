#pragma once

#include "shared/protocol.h"
#include <string>
#include <vector>
#include <set>
#include <memory>
#include <chrono>

namespace fbx::xserver {

class IpcSocket;

struct Notification {
    uint32_t id;
    std::string title;
    std::string message;
    std::string app_name;
    std::chrono::steady_clock::time_point expires;
};

class NotificationService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept;
    static const std::vector<Notification>& getActiveNotifications() noexcept;
    // Prune expired notifications and broadcast Dismiss for each removed one
    static void cleanup() noexcept;

private:
    static std::vector<Notification> active_notifications_;
    static std::set<std::string>     muted_apps_;
    static uint32_t next_id_;

    static void broadcastMutedApps() noexcept;
};

} // namespace fbx::xserver
