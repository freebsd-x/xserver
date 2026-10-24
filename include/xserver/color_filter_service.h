#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <array>
#include <functional>

namespace fbx::xserver {

// com.freebsdx.ColorFilter — colour-blindness / monochrome screen filter, applied
// as a 3x3 colour matrix (DRM CRTC CTM) via the hook installed by init().
class ColorFilterService {
public:
    // Install the apply hook (wired to DrmDevice::setColorMatrix in main) and push
    // the current matrix immediately. nullptr detaches.
    static void init(std::function<void(const std::array<double, 9>&)> apply) noexcept;

    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    // Pure: the row-major 3x3 colour matrix for a given state (identity when the
    // filter is off; lerped toward the filter by intensity%). Test seam.
    [[nodiscard]] static std::array<double, 9> computeMatrix(
        const shared::ColorFilterState& st) noexcept;
};

}  // namespace fbx::xserver
