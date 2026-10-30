#include "xserver/headless_service.h"
#include <iostream>
#include "xserver/remote_desktop_service.h"
#include "shared/logger.h"
#include <cstring>
#include <unistd.h>

namespace fbx::xserver {

// ── Static member definitions ─────────────────────────────────────────────────

VulkanCompositor*     HeadlessService::comp_{nullptr};
uint32_t              HeadlessService::width_{1920};
uint32_t              HeadlessService::height_{1080};
std::atomic<uint32_t> HeadlessService::frame_count_{0};
bool                  HeadlessService::headless_{false};
std::function<std::pair<float, float>()> HeadlessService::pointer_query_{};
std::function<uint32_t()>                 HeadlessService::cursor_shape_query_{};

// ── init ──────────────────────────────────────────────────────────────────────

void HeadlessService::init(VulkanCompositor* comp,
                           uint32_t w, uint32_t h) noexcept {
    comp_     = comp;
    width_    = w;
    height_   = h;
    headless_ = true;
    LOG_INFO("HeadlessService: virtual output {}x{}", w, h);
}

// ── ProcessMessage ────────────────────────────────────────────────────────────

bool HeadlessService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                     const shared::XBusMessage& msg) noexcept {
    const auto op = static_cast<shared::HeadlessOpcode>(msg.opcode_);
    shared::XBusMessage resp = msg; // echo header (service_name + opcode)

    // ── GetInfo ───────────────────────────────────────────────────────────────
    if (op == shared::HeadlessOpcode::GetInfo) {
        shared::HeadlessInfo info{};
        info.headless_    = headless_ ? 1u : 0u;
        info.width_       = width_;
        info.height_      = height_;
        info.frame_count_ = frame_count_.load(std::memory_order_relaxed);
        std::memcpy(resp.payload_, &info, sizeof(info));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        return true;
    }

    // Everything below drives or reads the screen (inject input, capture the
    // frame, read the pointer) — the test harness's tools. On a live seat they
    // would let any app type into other windows or capture the screen, so
    // they exist only on a --headless compositor.
    if (!headless_) {
        std::cerr << "[Headless] DENIED: opcode " << msg.opcode_ << " on a live seat\n";
        return true;
    }

    // ── CaptureFrame ──────────────────────────────────────────────────────────
    // Renders the current composited frame into an anonymous shared-memory FD
    // (same wire format as com.freebsdx.Screencopy / CaptureOutput).
    if (op == shared::HeadlessOpcode::CaptureFrame) {
        if (!comp_) {
            // No compositor — return zero ScreencopyFrameInfo as failure signal
            shared::ScreencopyFrameInfo fail{};
            std::memcpy(resp.payload_, &fail, sizeof(fail));
            (void)client->sendMessage(
                {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
            return true;
        }

        auto res = comp_->captureToShm(0);
        if (res.has_value()) {
            auto [shm_fd, frame_info] = std::move(res.value());
            std::memcpy(resp.payload_, &frame_info, sizeof(frame_info));
            // shm_fd is sent as SCM_RIGHTS; kernel duplicates it for the recipient
            (void)client->sendMessage(
                {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)}, shm_fd);
            ::close(shm_fd);
        } else {
            LOG_ERROR("HeadlessService: captureToShm failed");
            shared::ScreencopyFrameInfo fail{};
            std::memcpy(resp.payload_, &fail, sizeof(fail));
            (void)client->sendMessage(
                {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        }
        return true;
    }

    // ── InjectInput ─────────────────────────────────────────────────────────────
    // Feed a synthetic InputMessage into the compositor's real injected-input
    // path (drained each frame in the main loop), so tests can drive pointer /
    // keyboard input headlessly. No response.
    if (op == shared::HeadlessOpcode::InjectInput) {
        shared::InputMessage in{};
        std::memcpy(&in, msg.payload_, sizeof(in));
        RemoteDesktopService::pushInjectedInput(in);
        return true;
    }

    // ── GetCursorShape ──────────────────────────────────────────────────────────
    if (op == shared::HeadlessOpcode::GetCursorShape) {
        uint32_t shape = cursor_shape_query_ ? cursor_shape_query_() : 0u;
        std::memcpy(resp.payload_, &shape, sizeof(shape));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        return true;
    }

    // ── GetPointer ──────────────────────────────────────────────────────────────
    if (op == shared::HeadlessOpcode::GetPointer) {
        shared::HeadlessPointer p{};
        if (pointer_query_) {
            auto [x, y] = pointer_query_();
            p.x_ = x;
            p.y_ = y;
        }
        std::memcpy(resp.payload_, &p, sizeof(p));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        return true;
    }

    return false; // unknown opcode
}

} // namespace fbx::xserver
