#pragma once
#include "shared/protocol.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fbx::xserver {
class IpcSocket;

// com.freebsdx.DefaultApps — the handler app for each file-type category.
// Per-page request/response: ListDefaults(page) returns the category at that
// index with its current handler; CycleDefault(category) advances that
// category's handler to the next candidate (wrapping) and broadcasts
// DefaultsChanged so the shell re-fetches. Backed by a seeded in-memory table.
class DefaultAppsService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

private:
    struct Category {
        std::string              name;
        std::vector<std::string> handlers;  // candidate apps, [0] is the default
        uint32_t                 idx;        // currently selected handler
    };

    static std::mutex             mutex_;
    static std::vector<Category>  categories_;
    static bool                   loaded_;

    static void load() noexcept;   // seed defaults once
    static void broadcastChanged() noexcept;
};

} // namespace fbx::xserver
