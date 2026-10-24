#pragma once

#include "shared/protocol.h"
#include <memory>

namespace fbx::xserver {

class IpcSocket;

// com.freebsdx.Backlight — reads/writes hw.acpi.video.lcd0.brightness.
// Also exposes a static step() entry point called directly by the
// input handler in main.cc when KEY_BRIGHTNESSUP / KEY_BRIGHTNESSDOWN
// are pressed, so no client socket is involved.
class BacklightService {
public:
    static void ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    // Called from main.cc key handler (no IPC round-trip).
    // delta > 0 → step up, delta < 0 → step down (clamped to [0,100]).
    static void step(int delta) noexcept;

    static uint32_t getLevel() noexcept;

private:
    static void writeLevel(uint32_t level) noexcept;

    // Cached level; 0xFF = uninitialised (read from sysctl on first use).
    static uint32_t current_level_;
};

} // namespace fbx::xserver
