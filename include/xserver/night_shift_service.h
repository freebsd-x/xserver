#pragma once
#include "shared/protocol.h"
#include <memory>
#include <atomic>

namespace fbx::xserver {
class IpcSocket;

class NightShiftService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    static bool     isEnabled()  noexcept { return enabled_.load() != 0; }
    static uint32_t intensity()  noexcept { return intensity_.load(); }
    static bool     isAutoMode() noexcept { return auto_mode_.load() != 0; }

    // Called from compositor's 1-minute tick to auto-enable/disable by sun position.
    static void pollAutoMode() noexcept;

private:
    static std::atomic<uint32_t> enabled_;
    static std::atomic<uint32_t> intensity_;
    static std::atomic<uint32_t> auto_mode_;

    static void broadcastState() noexcept;
};

} // namespace fbx::xserver
