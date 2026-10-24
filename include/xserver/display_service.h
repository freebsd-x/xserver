#pragma once

#include "shared/protocol.h"
#include "xserver/window_stack.h"
#include <memory>
#include <vector>

namespace fbx::xserver {

class IpcSocket;
class DrmDevice;

class DisplayService {
public:
    // Update stored output table (call on startup and hot-plug before broadcasting).
    static void setOutputs(const std::vector<OutputInfo>& layout) noexcept;

    // Apply an output scale and notify clients (used by DisplayOpcode::SetOutputScale
    // and by com.freebsdx.DisplayScale). Recomputes logical dims + broadcasts
    // OutputChanged so apps re-layout. No-op on null stack / non-positive scale.
    static void applyOutputScale(WindowStack* stack, size_t idx, float scale) noexcept;

    // Handle a com.freebsdx.Display XBus message from a client.
    // drm may be nullptr (e.g. in headless/test mode); VRR ops will be no-ops.
    static void ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg,
                               WindowStack* stack,
                               DrmDevice* drm = nullptr) noexcept;

    // Broadcast OutputChanged to all registered clients.
    static void BroadcastOutputChanged() noexcept;

private:
    [[nodiscard]] static shared::DisplayOutputList buildOutputList() noexcept;

    static std::vector<OutputInfo> g_outputs_;
};

} // namespace fbx::xserver
