#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include "xserver/display_arrange.h"
#include <functional>
#include <memory>
#include <vector>

namespace fbx::xserver {

// com.freebsdx.DisplayArrange — multi-monitor layout. GetLayout reports the
// compositor's live outputs (supplied via init() from window_stack; empty when
// no provider is set, e.g. headless/tests). SetPrimary/SetMirror persist the
// intent + broadcast LayoutChanged; the actual DRM CRTC reconfiguration is a
// follow-up (and moot on a single-display seat).
class DisplayArrangeService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    // Wire the live-output source (from main.cc, reading window_stack).
    static void init(std::function<std::vector<disparrange::ArrangeOutput>()> provider) noexcept;
};

}  // namespace fbx::xserver
