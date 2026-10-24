#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <array>
#include <functional>

namespace fbx::xserver {

// com.freebsdx.NightLight — warm-tint toggle (GetState / Set* → persist +
// StateChanged broadcast). The tint is applied as a diagonal colour matrix via the
// hook installed by init() (reusing the DRM CTM path shared with ColorFilter).
class NightLightService {
public:
    // Install the tint-apply hook (wired to the colour pipeline in main) and push
    // the current matrix immediately. nullptr detaches.
    static void init(std::function<void(const std::array<double, 9>&)> apply) noexcept;

    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    // Pure: diagonal RGB tint matrix for a state (identity when disabled; warmer =
    // lower green/blue). 6500K ≈ neutral, 2700K ≈ warmest. Test seam.
    [[nodiscard]] static std::array<double, 9> computeMatrix(
        const shared::NightLightState& st) noexcept;
};

}  // namespace fbx::xserver
