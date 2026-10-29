#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <functional>
#include <array>

namespace fbx::xserver {

// com.freebsdx.ColorProfile — ICC display color profile (sRGB/Display P3/Adobe
// RGB). GetProfile replies with the config-backed state (profile + derived
// name); SetProfile persists and broadcasts ProfileChanged (no direct reply per
// contract). The actual color-managed transform needs a CMS/CTM pipeline →
// follow-up; this persists + confirms the selection.
class ColorProfileService {
public:
    // Install the hook that puts a profile's matrix into the CRTC colour
    // pipeline (main), and apply the saved profile.
    static void init(std::function<void(const std::array<double, 9>&)> apply) noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
