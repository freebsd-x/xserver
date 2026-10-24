#pragma once
#include "shared/protocol.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fbx::xserver {
class IpcSocket;

// com.freebsdx.TrustedDevices — devices allowed to pair/sync without a prompt.
// Per-page request/response: ListDevices(page) returns the single device at that
// index (valid_/has_more_ drive the shell's page walk); SetTrust flips the
// trusted flag for a named device and broadcasts DevicesChanged so the shell
// re-fetches. Backed by a seeded in-memory list (no real pairing subsystem yet).
class TrustedDevicesService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

private:
    struct Device { std::string name; uint32_t trusted; };

    static std::mutex           mutex_;
    static std::vector<Device>  devices_;
    static bool                 loaded_;

    static void load() noexcept;   // seed defaults once
    static void broadcastChanged() noexcept;
};

} // namespace fbx::xserver
