#pragma once

#include "xserver/unique_fd.h"
#include "xserver/xbus_message_pool.h"
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <expected>
#include <system_error>

namespace fbx::xserver {

class IpcSocket {
public:
    explicit IpcSocket(UniqueFd fd) noexcept;
    explicit IpcSocket(const std::string& path) noexcept;

    bool connect() noexcept;
    [[nodiscard]] int fd() const noexcept { return fd_.get(); }

    // Send a payload with an optional file descriptor.
    // Times the sendmsg(2) call with CLOCK_MONOTONIC and logs a rolling average
    // every kSendLogInterval calls.
    std::expected<void, std::error_code> sendMessage(std::span<const uint8_t> data, int fd_to_send = -1) const noexcept;

    // Receive into a pool slot — zero heap allocation on the hot path.
    // The returned PooledSlot releases back to the pool when it goes out of scope.
    // Distinguishes EAGAIN (non-fatal) from disconnect errors via error_code.
    [[nodiscard]] std::expected<XBusMessagePool::PooledSlot, std::error_code>
        receiveIntoSlot(XBusMessagePool& pool, UniqueFd& out_fd) const noexcept;

    // Legacy API — allocates a std::vector<uint8_t>. Kept for callers outside the
    // main dispatch loop (e.g., tests, SDK client library).
    std::expected<std::pair<std::vector<uint8_t>, UniqueFd>, std::error_code> receiveMessage() const noexcept;

    static constexpr uint32_t kSendLogInterval = 10'000; // log every N sendmsg calls

    void setTrusted(bool trusted) noexcept { is_trusted_ = trusted; }
    [[nodiscard]] bool isTrusted() const noexcept { return is_trusted_; }

    // Set when the client sends SandboxOpcode::EnterSandbox after calling cap_enter().
    // Sandboxed clients may not alter system-wide settings (theme, display, VRR, etc.).
    void markSandboxed() noexcept { is_sandboxed_ = true; }
    [[nodiscard]] bool isSandboxed() const noexcept { return is_sandboxed_; }

    // Security context identity — set when client sends SecurityContextOpcode::SetContext.
    void setAppId(std::string_view app_id) noexcept { app_id_ = std::string(app_id); }
    void setSandboxEngine(std::string_view engine) noexcept { sandbox_engine_ = std::string(engine); }
    void setInstanceId(std::string_view id) noexcept { instance_id_ = std::string(id); }
    [[nodiscard]] const std::string& appId() const noexcept { return app_id_; }
    [[nodiscard]] const std::string& sandboxEngine() const noexcept { return sandbox_engine_; }
    [[nodiscard]] const std::string& instanceId() const noexcept { return instance_id_; }

private:
    UniqueFd fd_;
    std::string socket_path_;
    bool is_trusted_{false};
    bool is_sandboxed_{false};
    std::string app_id_;
    std::string sandbox_engine_;
    std::string instance_id_;
};

} // namespace fbx::xserver
