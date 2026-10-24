#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include "xserver/vulkan_compositor.h"
#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace fbx::xserver {

// ── HeadlessService — com.freebsdx.Headless ──────────────────────────────────
// Active on any xserver build but only returns headless_=1 when the compositor
// was started with --headless (no DRM/KMS device).
//
// Opcodes (see shared::HeadlessOpcode):
//   GetInfo      (0x01) → HeadlessInfo
//   CaptureFrame (0x02) → ScreencopyFrameInfo + shm fd via SCM_RIGHTS

class HeadlessService {
public:
    // Call once after VulkanCompositor::create() when --headless is active.
    // w/h are the virtual output dimensions (from --headless-size or defaults).
    static void init(VulkanCompositor* comp,
                     uint32_t w, uint32_t h) noexcept;

    [[nodiscard]] static bool     isHeadless()  noexcept { return headless_; }
    [[nodiscard]] static uint32_t width()        noexcept { return width_;   }
    [[nodiscard]] static uint32_t height()       noexcept { return height_;  }

    // Incremented by the render timer after each composite cycle in headless mode.
    static void incrementFrameCount() noexcept { ++frame_count_; }

    // Dispatch an incoming XBus message.  Returns true if consumed.
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    // Test-harness hook: main.cc registers a getter for the current cursor
    // position so GetPointer can report it (the position lives in the main
    // event loop, not here).
    static void setPointerQuery(std::function<std::pair<float, float>()> q) noexcept {
        pointer_query_ = std::move(q);
    }
    // Getter for the last cursor shape the compositor decided on (wp_cursor_shape
    // value), so GetCursorShape can report it headlessly (no DRM plane needed).
    static void setCursorShapeQuery(std::function<uint32_t()> q) noexcept {
        cursor_shape_query_ = std::move(q);
    }

private:
    HeadlessService() = delete;

    static VulkanCompositor*       comp_;
    static uint32_t                width_;
    static uint32_t                height_;
    static std::atomic<uint32_t>   frame_count_;
    static bool                    headless_;
    static std::function<std::pair<float, float>()> pointer_query_;
    static std::function<uint32_t()>                 cursor_shape_query_;
};

} // namespace fbx::xserver
