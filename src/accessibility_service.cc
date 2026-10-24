#include "xserver/accessibility_service.h"
#include "xserver/ipc_socket.h"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace fbx::xserver {

// ── Static storage ────────────────────────────────────────────────────────────

std::vector<std::weak_ptr<IpcSocket>> AccessibilityService::subscribers_;
std::unordered_map<uint32_t, std::vector<shared::AccessibilityTreeNode>>
    AccessibilityService::surface_trees_;

// ── Internal helpers ──────────────────────────────────────────────────────────

void AccessibilityService::broadcast(const shared::XBusMessage& msg) noexcept {
    // Send to all live subscribers; prune dead weak_ptrs in-place.
    subscribers_.erase(
        std::remove_if(subscribers_.begin(), subscribers_.end(),
            [&](const std::weak_ptr<IpcSocket>& wp) {
                auto client = wp.lock();
                if (!client) return true; // dead — remove
                (void)client->sendMessage(
                    {reinterpret_cast<const uint8_t*>(&msg), sizeof(msg)});
                return false;
            }),
        subscribers_.end());
}

void AccessibilityService::sendStoredTree(const std::shared_ptr<IpcSocket>& client,
                                          uint32_t surface_id) noexcept {
    auto it = surface_trees_.find(surface_id);
    if (it == surface_trees_.end()) return;

    const auto& nodes = it->second;
    uint32_t total = static_cast<uint32_t>(nodes.size());
    uint32_t pages = (total + 2) / 3; // ceil(total/3)

    for (uint32_t p = 0; p < pages; ++p) {
        shared::AccessibilityTreePage page{};
        page.surface_id_  = surface_id;
        page.total_nodes_ = total;
        page.page_index_  = p;

        uint32_t base = p * 3;
        uint32_t count = std::min(3u, total - base);
        page.node_count_ = count;
        for (uint32_t i = 0; i < count; ++i) {
            page.nodes_[i] = nodes[base + i];
        }

        shared::XBusMessage reply{};
        std::strncpy(reply.service_name_, "com.freebsdx.Accessibility",
                     sizeof(reply.service_name_) - 1);
        reply.opcode_ = static_cast<uint32_t>(shared::AccessibilityOpcode::UpdateTree);
        std::memcpy(reply.payload_, &page, sizeof(page));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void AccessibilityService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                          const shared::XBusMessage& msg) noexcept {
    auto opcode = static_cast<shared::AccessibilityOpcode>(msg.opcode_);

    switch (opcode) {
    case shared::AccessibilityOpcode::Subscribe:
        // Register as a broadcast subscriber (skip duplicates).
        for (const auto& wp : subscribers_) {
            if (wp.lock() == client) return; // already registered
        }
        subscribers_.emplace_back(client);
        std::cout << "[a11y] Screen reader subscribed\n";
        return;

    case shared::AccessibilityOpcode::Unsubscribe:
        subscribers_.erase(
            std::remove_if(subscribers_.begin(), subscribers_.end(),
                [&](const std::weak_ptr<IpcSocket>& wp) {
                    return wp.lock() == client;
                }),
            subscribers_.end());
        std::cout << "[a11y] Screen reader unsubscribed\n";
        return;

    case shared::AccessibilityOpcode::Announce: {
        // App pushes text to speak — broadcast to all subscribers.
        shared::AccessibilityAnnounce ann{};
        std::memcpy(&ann, msg.payload_, sizeof(ann));
        ann.text_[sizeof(ann.text_) - 1] = '\0';

        shared::XBusMessage bcast{};
        std::strncpy(bcast.service_name_, "com.freebsdx.Accessibility",
                     sizeof(bcast.service_name_) - 1);
        bcast.opcode_ = static_cast<uint32_t>(shared::AccessibilityOpcode::Announce);
        std::memcpy(bcast.payload_, &ann, sizeof(ann));
        std::cout << "[a11y] Announce: " << ann.text_ << "\n";
        broadcast(bcast);
        return;
    }

    case shared::AccessibilityOpcode::UpdateTree: {
        // App pushes a page of its semantic tree — accumulate per surface.
        shared::AccessibilityTreePage page{};
        std::memcpy(&page, msg.payload_, sizeof(page));

        // Clamp client-supplied counts: total_nodes_ bounds an allocation (reject
        // implausible trees to avoid a huge resize / OOM), and node_count_ must
        // not exceed the fixed nodes_[3] array (else the copy reads past payload_).
        constexpr uint32_t kMaxTreeNodes = 8192;
        if (page.total_nodes_ > kMaxTreeNodes) return;
        uint32_t node_count = page.node_count_ > 3 ? 3 : page.node_count_;

        auto& nodes = surface_trees_[page.surface_id_];
        // First page of a new tree — resize and reset.
        if (page.page_index_ == 0) {
            nodes.resize(page.total_nodes_);
        }

        uint32_t base = page.page_index_ * 3;
        for (uint32_t i = 0; i < node_count && (base + i) < nodes.size(); ++i) {
            nodes[base + i] = page.nodes_[i];
        }

        // When the last page arrives, broadcast the whole tree to subscribers.
        uint32_t pages_needed = (page.total_nodes_ + 2) / 3;
        if (page.page_index_ + 1 == pages_needed) {
            shared::XBusMessage bcast{};
            std::strncpy(bcast.service_name_, "com.freebsdx.Accessibility",
                         sizeof(bcast.service_name_) - 1);
            bcast.opcode_ = static_cast<uint32_t>(shared::AccessibilityOpcode::UpdateTree);
            for (const auto& wp : subscribers_) {
                auto sub = wp.lock();
                if (sub) sendStoredTree(sub, page.surface_id_);
            }
        }
        return;
    }

    case shared::AccessibilityOpcode::QueryTree: {
        // Screen reader requests the stored tree for a specific surface.
        uint32_t surface_id = 0;
        std::memcpy(&surface_id, msg.payload_, sizeof(surface_id));
        sendStoredTree(client, surface_id);
        return;
    }

    default:
        return;
    }
}

void AccessibilityService::OnClientDisconnected(
        const std::shared_ptr<IpcSocket>& client) noexcept {
    subscribers_.erase(
        std::remove_if(subscribers_.begin(), subscribers_.end(),
            [&](const std::weak_ptr<IpcSocket>& wp) {
                return wp.lock() == client;
            }),
        subscribers_.end());
}

void AccessibilityService::BroadcastAnnounce(uint32_t surface_id,
                                             const std::string& text) noexcept {
    shared::AccessibilityAnnounce ann{};
    ann.surface_id_ = surface_id;
    std::size_t len = std::min(text.size(), sizeof(ann.text_) - 1);
    std::memcpy(ann.text_, text.data(), len);
    ann.text_[len] = '\0';

    shared::XBusMessage msg{};
    std::strncpy(msg.service_name_, "com.freebsdx.Accessibility",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::AccessibilityOpcode::Announce);
    std::memcpy(msg.payload_, &ann, sizeof(ann));
    broadcast(msg);
}

} // namespace fbx::xserver
