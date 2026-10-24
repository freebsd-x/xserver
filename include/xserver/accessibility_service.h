#pragma once

#include "shared/protocol.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace fbx::xserver {

class IpcSocket;

// AccessibilityService — compositor-side hub for screen reader support.
//
// Flow:
//   1. Screen reader app sends com.freebsdx.Accessibility/Subscribe → registered
//      as a broadcast subscriber.
//   2. GUI apps call XApplication::announce(text) → Announce opcode delivered
//      here → AccessibilityAnnounce broadcast to every subscriber.
//   3. GUI apps optionally call XApplication::updateAccessibilityTree() →
//      UpdateTree pages arrive here and are stored per-surface.
//   4. Screen reader can send QueryTree → stored pages replayed back to it.
//
class AccessibilityService {
public:
    // XBus handler for "com.freebsdx.Accessibility" messages.
    static void ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    // Called by the compositor when a surface loses its connection, so its tree
    // and any subscriber entry can be cleaned up.
    static void OnClientDisconnected(const std::shared_ptr<IpcSocket>& client) noexcept;

    // Compositor-generated announcement (e.g. focus moved to a new surface).
    static void BroadcastAnnounce(uint32_t surface_id,
                                  const std::string& text) noexcept;

private:
    static void broadcast(const shared::XBusMessage& msg) noexcept;
    static void sendStoredTree(const std::shared_ptr<IpcSocket>& client,
                               uint32_t surface_id) noexcept;

    // Connected screen reader clients waiting for broadcasts.
    static std::vector<std::weak_ptr<IpcSocket>> subscribers_;

    // Latest semantic tree pages per surface (key = surface_id).
    static std::unordered_map<uint32_t,
               std::vector<shared::AccessibilityTreeNode>> surface_trees_;
};

} // namespace fbx::xserver
