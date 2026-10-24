#pragma once

#include "shared/protocol.h"
#include "xserver/unique_fd.h"
#include <memory>
#include <span>
#include <optional>
#include <functional>

namespace fbx::xserver {
    class IpcSocket;
    class VulkanCompositor;
    class WindowStack;
    class DrmDevice;
}

namespace fbx::xserver {

// ─── DnD state (shared between xbus_dispatcher and main.cc) ──────────────────

struct DnDDragState {
    bool     active{false};
    uint32_t source_surface{0};
    std::shared_ptr<IpcSocket> source_client;
    char     mime_type[64]{};
    char     data[184]{};
    uint32_t current_target{UINT32_MAX};  // surface currently under cursor
};

DnDDragState& GetDnDState() noexcept;
void          ClearDnDState() noexcept;

// Send a DnD XBus message to a specific client socket.
void DnDSendToClient(std::shared_ptr<IpcSocket> client,
                     shared::DnDOpcode opcode,
                     float x, float y,
                     const char* mime_type,
                     const char* data = nullptr) noexcept;

void RegisterClientForBroadcast(std::shared_ptr<IpcSocket> client);
void InjectInputAction(const shared::InputMessage& input_msg);
void BroadcastNotification(const shared::NotificationPayload& payload);
void BroadcastNotificationDismiss(uint32_t id);
void BroadcastXBusMessage(const shared::XBusMessage& msg);
void BroadcastPresentFeedback(const shared::PresentFeedback& fb);
void BroadcastWorkspaceChanged(uint32_t index) noexcept;
void BroadcastSwitcherUpdate(const shared::SwitcherPayload& payload) noexcept;
void BroadcastSwitcherDismiss() noexcept;
void BroadcastWindowListUpdate(const shared::WindowListPayload& payload) noexcept;
void RebuildAndBroadcastWindowList(const WindowStack* stack) noexcept;
void LoadTheme();

struct XBusResult {
    bool consumed;
    std::optional<shared::InputMessage> injected_input;
};

class WindowStack;

class XBusDispatcher {
public:
    using ShutdownCallback      = std::function<void()>;
    using CursorLockCallback    = std::function<void(bool locked)>;
    using CursorShapeCallback   = std::function<void(uint32_t shape)>;
    using RaiseCallback         = std::function<void(uint32_t surface_id)>;
    using RedrawCallback        = std::function<void()>;

    static void OnShutdown(ShutdownCallback cb)        { shutdown_cb_      = std::move(cb); }
    static void OnCursorLock(CursorLockCallback cb)    { cursor_lock_cb_   = std::move(cb); }
    static void OnCursorShape(CursorShapeCallback cb)  { cursor_shape_cb_  = std::move(cb); }
    static void OnRaise(RaiseCallback cb)              { raise_cb_         = std::move(cb); }
    // Fired when a server-side change (e.g. wallpaper) needs the scene recomposited.
    static void OnRedraw(RedrawCallback cb)            { redraw_cb_        = std::move(cb); }

    // Process an incoming message. Returns a result indicating if it was an XBus message and any injected input.
    [[nodiscard]] static XBusResult ProcessMessage(std::shared_ptr<IpcSocket> client, const std::span<const uint8_t>& data, UniqueFd& fd_opt, VulkanCompositor* comp = nullptr, WindowStack* stack = nullptr, bool has_focus = false, DrmDevice* drm = nullptr) noexcept;

private:
    static ShutdownCallback    shutdown_cb_;
    static CursorLockCallback  cursor_lock_cb_;
    static CursorShapeCallback cursor_shape_cb_;
    static RaiseCallback       raise_cb_;
    static RedrawCallback      redraw_cb_;
};

} // namespace fbx::xserver
