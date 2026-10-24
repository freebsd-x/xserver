#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <shared/protocol.h>
#include "xserver/ipc_socket.h"

namespace fbx {

using xserver::IpcSocket;

class DiskService {
public:
    static void init() noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

private:
    // Disk / partition enumeration
    static std::vector<shared::DiskInfo>      enumerateDisks() noexcept;
    static std::vector<shared::PartitionInfo> listPartitions(const std::string& disk) noexcept;
    static std::vector<shared::ZpoolInfo>     listZpools() noexcept;

    // Input validation (shell-injection prevention)
    static bool isValidDevice(const std::string& dev) noexcept;
    static bool isValidPath(const std::string& path) noexcept;

    // Response helpers
    static void sendStatus(std::shared_ptr<IpcSocket> client,
                           bool ok, const std::string& msg) noexcept;

    static std::mutex mutex_;
};

} // namespace fbx
