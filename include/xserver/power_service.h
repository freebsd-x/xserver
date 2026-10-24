#pragma once

#include "shared/protocol.h"
#include <memory>

namespace fbx::xserver {

class IpcSocket;

// com.freebsdx.Power — ACPI power management: suspend/hibernate/reboot,
// lid-close policy, sleep timeout, and AC/battery state broadcasting.
class PowerService {
public:
    static void ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    // Called from the main event loop timer (~30 s) to detect AC↔battery
    // transitions and broadcast PowerChanged to all connected clients.
    static void pollAndBroadcast() noexcept;

    static shared::PowerState getPowerState() noexcept;
    static shared::PowerLidAction getLidPolicy() noexcept;
    static uint32_t getSleepTimeoutSeconds() noexcept;

private:
    static shared::PowerLidAction lid_policy_;
    static uint32_t               sleep_timeout_seconds_;
    static uint32_t               last_on_ac_;  // 0xFF = uninitialised
};

} // namespace fbx::xserver
