#pragma once
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <memory>
#include <unordered_map>
#include <string>

namespace fbx::xserver {

// Per-surface security context stored for bridge clients
// (multiple surfaces share one IpcSocket, so we need per-surface overrides).
struct SurfaceSecurityContext {
    std::string app_id;
    std::string sandbox_engine;
    std::string instance_id;
};

class SecurityContextService {
public:
    static void ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    // Return the app_id for a surface: per-surface override first, then socket-level.
    static std::string GetAppId(const IpcSocket& socket, uint32_t surface_id) noexcept;

    // Called by the compositor when a surface is closed, to prune stale entries.
    static void OnSurfaceClosed(uint32_t surface_id) noexcept;

private:
    // surface_id → context (bridge clients only; surface_id 0 is stored on IpcSocket directly)
    static std::unordered_map<uint32_t, SurfaceSecurityContext> surface_contexts_;
};

} // namespace fbx::xserver
