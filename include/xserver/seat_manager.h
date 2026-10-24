#pragma once

#include "xserver/unique_fd.h"
#include <string>
#include <expected>
#include <system_error>
#include <memory>

extern "C" {
#include <libseat.h>
}

namespace fbx::xserver {

struct SeatDevice {
    UniqueFd fd_;
    int device_id_;
};

class ISeatManager {
public:
    virtual ~ISeatManager() noexcept = default;

    virtual std::expected<SeatDevice, std::error_code> openDevice(const std::string& path) noexcept = 0;
    virtual std::expected<void, std::error_code> closeDevice(int device_id) noexcept = 0;
    [[nodiscard]] virtual int getFd() const noexcept = 0;
    virtual std::expected<void, std::error_code> dispatch(int timeout_ms = 0) noexcept = 0;

    // Enter shutdown: subsequent closeDevice() calls become no-ops so a blocking
    // seatd round-trip can't wedge teardown. Default no-op (the broker backend
    // reclaims fds on socket close); the libseat backend overrides it.
    virtual void beginShutdown() noexcept {}
};

class SeatManager : public ISeatManager {
public:
    // Factory method using std::expected
    static std::expected<std::unique_ptr<SeatManager>, std::error_code> create() noexcept;

    ~SeatManager() noexcept override;

    SeatManager(const SeatManager&) = delete;
    SeatManager& operator=(const SeatManager&) = delete;
    SeatManager(SeatManager&&) = delete;
    SeatManager& operator=(SeatManager&&) = delete;

    // Open a device securely via seatd
    std::expected<SeatDevice, std::error_code> openDevice(const std::string& path) noexcept override;

    // Close a device securely via seatd
    std::expected<void, std::error_code> closeDevice(int device_id) noexcept override;

    // Get the libseat internal FD for the kqueue event loop
    [[nodiscard]] int getFd() const noexcept override;

    // Dispatch seat events (enable/disable)
    std::expected<void, std::error_code> dispatch(int timeout_ms = 0) noexcept override;

    static void onEnableSeat(libseat* seat, void* userdata) noexcept;
    static void onDisableSeat(libseat* seat, void* userdata) noexcept;

    // Enter shutdown: subsequent closeDevice() calls become no-ops. During
    // process teardown the event loop that pumps seatd is gone, so the blocking
    // libseat_close_device round-trip would wedge forever (freezing the seat).
    // Device fds are reclaimed by the OS on exit and seatd sees our disconnect.
    void beginShutdown() noexcept override { shutting_down_ = true; }

private:
    SeatManager() noexcept = default;

    libseat* seat_{nullptr};
    bool is_active_{false};
    bool shutting_down_{false};
};

} // namespace fbx::xserver
