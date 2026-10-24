#pragma once
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <string>
#include <atomic>

namespace fbx::xserver {

// Handles com.freebsdx.XWayland XBus messages.
// Stores the DISPLAY env set by Xwayland and broadcasts it to all clients.
class XWaylandService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    static bool isRunning() noexcept;
    static const std::string& display() noexcept; // e.g., ":1"
    static int pid() noexcept;

private:
    static void broadcastStatus() noexcept;

    static std::atomic<bool> running_;
    static std::atomic<int>  pid_;
    static std::string       display_;
};

} // namespace fbx::xserver
