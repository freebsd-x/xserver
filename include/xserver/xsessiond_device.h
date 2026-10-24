#pragma once

#include "xserver/seat_manager.h"   // ISeatManager, SeatDevice

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <system_error>

namespace fbx::xserver {

// An ISeatManager that brokers device fds through xsessiond's native protocol
// (OpenDevice/CloseDevice + SCM_RIGHTS) instead of talking to seatd via libseat.
// The root daemon open()s the allowlisted DRM/evdev node and passes the fd back;
// the unprivileged compositor never opens a privileged node itself.
//
// Phase 2a landed the device *brokering* half (OpenDevice/CloseDevice + fd
// passing). Phase 2b adds seat *activation*: when xsessiond runs as a libseat
// client of real seatd it relays VT-switch events as unsolicited SeatDisable/
// SeatEnable push datagrams on this connection. dispatch() (driven from the
// compositor's event loop via getFd()) drains those and invokes the handlers
// registered with setActivationHandlers(); on SeatDisable it also answers
// DisableAck so the daemon can hand DRM master back to seatd only after the
// compositor has quiesced the GPU. Selecting this manager on the live seat is
// still gated behind an opt-in flag; the default remains the libseat-backed
// SeatManager. See docs/xsessiond-design.md §4a/§4b, §6, §10.2.
class XsessiondDeviceManager : public ISeatManager {
public:
    using ActivationFn = std::function<void()>;

    // Connect to xsessiond and confirm it advertises the device capability.
    // Returns an error if the daemon is unreachable or lacks kCapDevice.
    static std::expected<std::unique_ptr<XsessiondDeviceManager>, std::error_code>
        create() noexcept;

    // Wrap an already-connected SEQPACKET fd without the Hello handshake. Takes
    // ownership of the fd. Used by tests (drive activation over a socketpair);
    // also handy if a caller has already connected + negotiated.
    static std::unique_ptr<XsessiondDeviceManager> adopt(int fd) noexcept;

    ~XsessiondDeviceManager() noexcept override;

    XsessiondDeviceManager(const XsessiondDeviceManager&) = delete;
    XsessiondDeviceManager& operator=(const XsessiondDeviceManager&) = delete;

    std::expected<SeatDevice, std::error_code>
        openDevice(const std::string& path) noexcept override;
    std::expected<void, std::error_code>
        closeDevice(int device_id) noexcept override;
    [[nodiscard]] int getFd() const noexcept override;
    // Drain any pending seat-activation push events and dispatch them to the
    // handlers below. Non-blocking when called on a readable fd (the event-loop
    // path). Returns an error only if the daemon connection is lost.
    std::expected<void, std::error_code> dispatch(int timeout_ms = 0) noexcept override;

    // Register the compositor's VT-switch reactions. on_disable must quiesce the
    // GPU (stop page-flipping) before it returns — dispatch() sends DisableAck
    // immediately afterward. on_enable reacquires + repaints. Either may be null.
    void setActivationHandlers(ActivationFn on_disable, ActivationFn on_enable) noexcept {
        on_disable_ = std::move(on_disable);
        on_enable_  = std::move(on_enable);
    }

private:
    explicit XsessiondDeviceManager(int fd) noexcept : fd_(fd) {}
    int          fd_;
    ActivationFn on_disable_;
    ActivationFn on_enable_;
};

} // namespace fbx::xserver
