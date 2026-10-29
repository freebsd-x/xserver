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
    // Post a notification from inside the compositor (same path as a client's
    // Post: mute, quiet hours, sound). Returns its id, or 0 if muted.
    static uint32_t post(shared::NotificationPayload payload) noexcept;
    // Prune expired notifications and broadcast Dismiss for each removed one;
    // release held notifications once quiet time is over.
    static void cleanup() noexcept;

private:
    static std::vector<Notification> active_notifications_;
    static std::set<std::string>     muted_apps_;
    static uint32_t next_id_;
    // Posted during Do Not Disturb / quiet hours; shown when that ends.
    static std::vector<shared::NotificationPayload> held_;
    static void deliver(const shared::NotificationPayload& payload) noexcept;

    static void broadcastMutedApps() noexcept;
};

} // namespace fbx::xserver
