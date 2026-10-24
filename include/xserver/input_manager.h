#pragma once

#include "xserver/seat_manager.h"
#include "shared/protocol.h"
#include <libinput.h>
#include <libudev.h>
#include <vector>
#include <unordered_map>
#include <expected>
#include <system_error>
#include <memory>

namespace fbx::xserver {

struct UdevDeleter { void operator()(udev* u) const noexcept; };
struct LibinputDeleter { void operator()(libinput* li) const noexcept; };

using UniqueUdev = std::unique_ptr<udev, UdevDeleter>;
using UniqueLibinput = std::unique_ptr<libinput, LibinputDeleter>;

class InputManager {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<InputManager>, std::error_code> create(ISeatManager* seatManager) noexcept;
    
    ~InputManager() noexcept = default;
    
    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;
    InputManager(InputManager&&) noexcept = default;
    InputManager& operator=(InputManager&&) noexcept = default;

    [[nodiscard]] int getFd() const noexcept;
    [[nodiscard]] std::expected<std::vector<shared::InputMessage>, std::error_code> dispatch() noexcept;

    // Must be called once after the DRM output resolution is known so that
    // touch/stylus absolute coordinates can be scaled to screen pixels.
    void setScreenSize(uint32_t w, uint32_t h) noexcept { screen_w_ = w; screen_h_ = h; }

    // Natural (reverse) scroll direction, applied to every device that supports
    // it — new devices at add time and existing ones immediately. Driven by
    // com.freebsdx.ScrollDir.
    void setNaturalScroll(bool enabled) noexcept;

    // Enable/disable touchpad devices (send-events mode), applied to every
    // touchpad — new devices at add time and existing ones immediately. Driven by
    // com.freebsdx.Touchpad.
    void setTouchpadEnabled(bool enabled) noexcept;

    // Pointer preferences (com.freebsdx.Pointer): speed 1-10, adaptive vs flat
    // acceleration, tap-to-click. Applied live + re-applied to hot-plugged devices.
    // (Natural scroll is handled by setNaturalScroll / com.freebsdx.ScrollDir.)
    void setPointerConfig(uint32_t speed, bool acceleration, bool tap_to_click) noexcept;

    static int openRestricted(const char* path, int flags, void* userData) noexcept;
    static void closeRestricted(int fd, void* userData) noexcept;

private:
    explicit InputManager(ISeatManager* seatManager) noexcept;
    std::expected<void, std::error_code> init() noexcept;

    // Apply per-device config (tap-to-click + natural scroll) on device add.
    void applyDeviceConfig(libinput_device* dev) noexcept;

    ISeatManager* seat_manager_{nullptr};

    // Devices are ref'd on add / unref'd on remove so natural-scroll changes can
    // be re-applied live.
    std::vector<libinput_device*> devices_;
    bool     natural_scroll_{false};
    bool     touchpad_enabled_{true};
    uint32_t pointer_speed_{5};        // 1-10
    bool     pointer_accel_{true};     // adaptive (vs flat)
    bool     pointer_tap_{true};       // tap-to-click on touchpads
    // A touchpad is identified by tap capability (matches applyDeviceConfig).
    static bool isTouchpad(libinput_device* dev) noexcept;
    // Apply the current pointer prefs to one device (accel speed/profile + tap).
    void applyPointerTo(libinput_device* dev) noexcept;
    UniqueUdev udev_{nullptr};

    uint32_t modifiers_{0};

    // Screen dimensions used to transform touch/stylus absolute coordinates.
    uint32_t screen_w_{1920};
    uint32_t screen_h_{1080};

    // Slot-0 tracking for backward-compat mouse-relative-delta synthesis.
    float touch0_prev_x_{-1.0f};
    float touch0_prev_y_{-1.0f};

    // Map to track the libseat device_id and retain UniqueFd ownership.
    std::unordered_map<int, SeatDevice> open_devices_;

    // MUST be the last-declared member so it is destroyed FIRST: libinput_unref()
    // calls back into closeRestricted(), which touches open_devices_ and
    // seat_manager_. If those were torn down before libinput, the callback would
    // dereference a freed hash table and crash — the segfault that previously
    // aborted shutdown before the CRTC could be restored.
    UniqueLibinput libinput_{nullptr};
};

} // namespace fbx::xserver
