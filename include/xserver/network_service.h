#pragma once

#include "shared/protocol.h"
#include <memory>
#include <vector>
#include <mutex>

namespace fbx::xserver {

class IpcSocket;

class NetworkService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    static shared::NetworkStatus getStatus() noexcept;

private:
    static void scanBackground() noexcept;
    static void connect(const shared::WifiConnectRequest& req) noexcept;
    static void disconnect() noexcept;

    static std::vector<shared::WifiNetwork> scan_cache_;
    // Guards scan_cache_: written by the detached scanBackground() thread, read
    // by ListNetworks on the main event-loop thread.
    static std::mutex scan_mutex_;
};

} // namespace fbx::xserver
