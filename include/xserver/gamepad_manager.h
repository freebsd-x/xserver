#pragma once

#include "shared/protocol.h"
#include <vector>
#include <string>
#include <unordered_map>

namespace fbx::xserver {

// Manages evdev-based gamepad devices found in /dev/input/event*.
// Detects devices that advertise EV_ABS + BTN_GAMEPAD capability,
// normalises axis ranges to [-1000, +1000], and converts raw
// struct input_event records to InputMessages.
class GamepadManager {
public:
    // Scan /dev/input/event* for gamepad devices.
    // Returns the count of gamepads found.
    int scan() noexcept;

    // File descriptors for all open gamepad devices (add to the event loop).
    [[nodiscard]] const std::vector<int>& fds() const noexcept { return fds_; }

    // Read and convert all pending events on fd into InputMessages.
    // fd must be one of the fds returned by fds().
    [[nodiscard]] std::vector<shared::InputMessage> read(int fd) noexcept;

    ~GamepadManager() noexcept;

private:
    struct AxisInfo {
        int32_t  minimum;
        int32_t  maximum;
        uint32_t logical_id; // 0=LX,1=LY,2=RX,3=RY,4=LT,5=RT,6=DpadX,7=DpadY
    };

    struct GamepadDev {
        int                                 fd{-1};
        uint32_t                            controller_idx{0};
        std::unordered_map<int, AxisInfo>   axes;  // evdev ABS_* → AxisInfo
    };

    // Map evdev ABS_* code → logical axis ID used in GamepadAxis messages.
    static int absToLogical(int abs_code) noexcept;

    std::vector<int>        fds_;
    std::vector<GamepadDev> devices_;
    uint32_t                next_idx_{0};
};

} // namespace fbx::xserver
