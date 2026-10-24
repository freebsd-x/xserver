#pragma once
#include "shared/protocol.h"
#include <memory>
#include <atomic>

namespace fbx::xserver {
class IpcSocket;

// com.freebsdx.TrueTone — adaptive white-balance that warms the display toward
// the ambient color temperature. Holds the authoritative enabled/strength state
// plus the measured ambient temperature (display-only; SetState never writes it).
class TrueToneService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    static bool     isEnabled()  noexcept { return enabled_.load() != 0; }
    static uint32_t strength()   noexcept { return strength_.load(); }
    static uint32_t ambientK()   noexcept { return ambient_k_.load(); }

private:
    static std::atomic<uint32_t> enabled_;
    static std::atomic<uint32_t> strength_;
    static std::atomic<uint32_t> ambient_k_;  // measured ambient temp (Kelvin)

    static void broadcastState() noexcept;
};

} // namespace fbx::xserver
