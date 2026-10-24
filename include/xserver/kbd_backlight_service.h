#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.KeyboardBacklight — enabled + brightness (0–100), config-backed.
// GetState/SetEnabled/SetBrightness ack with the state and broadcast
// StateChanged. No keyboard-backlight control is exposed on this hardware
// (no /dev/led, no acpi_asus), so the hardware effect is a follow-up — the
// selection persists + confirms.
class KbdBacklightService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
