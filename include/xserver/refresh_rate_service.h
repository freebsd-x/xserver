#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <functional>
namespace fbx::xserver {
// com.freebsdx.RefreshRate — primary-display refresh rate. The real DRM mode mask
// and an apply hook (a modeset) are supplied by init(); GetRate reports the live
// supported set, SetRate performs the modeset via the hook and persists on success.
class RefreshRateService {
public:
    // supported_mask: bits {0=60,1=120,2=144,3=240} this display offers.
    // apply(hz) performs the modeset and returns true on success. Pass {} / nullptr
    // to fall back to the 60 Hz floor with no real switching.
    static void init(uint32_t supported_mask, std::function<bool(uint32_t)> apply) noexcept;

    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};
}  // namespace fbx::xserver
