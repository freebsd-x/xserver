#include "xserver/gamepad_manager.h"
#include <dev/evdev/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <iostream>

namespace fbx::xserver {

// ── Axis logical ID mapping ────────────────────────────────────────────────

int GamepadManager::absToLogical(int abs_code) noexcept {
    switch (abs_code) {
    case ABS_X:     return 0; // Left stick X
    case ABS_Y:     return 1; // Left stick Y
    case ABS_RX:    return 2; // Right stick X
    case ABS_RY:    return 3; // Right stick Y
    case ABS_Z:     return 4; // Left trigger
    case ABS_RZ:    return 5; // Right trigger
    case ABS_HAT0X: return 6; // D-pad X
    case ABS_HAT0Y: return 7; // D-pad Y
    default:        return -1; // unmapped
    }
}

// ── Device detection ───────────────────────────────────────────────────────

int GamepadManager::scan() noexcept {
    DIR* dir = opendir("/dev/input");
    if (!dir) return 0;

    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        if (std::strncmp(de->d_name, "event", 5) != 0) continue;

        std::string path = std::string("/dev/input/") + de->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        // Check for EV_ABS capability
        uint8_t evbits[(EV_MAX / 8) + 1] = {};
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
            close(fd);
            continue;
        }
        bool has_abs = (evbits[EV_ABS / 8] >> (EV_ABS % 8)) & 1;
        bool has_key = (evbits[EV_KEY / 8] >> (EV_KEY % 8)) & 1;
        if (!has_abs || !has_key) { close(fd); continue; }

        // Check for BTN_GAMEPAD (0x130)
        uint8_t keybits[(KEY_MAX / 8) + 1] = {};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) {
            close(fd);
            continue;
        }
        bool has_gamepad_btn = (keybits[BTN_GAMEPAD / 8] >> (BTN_GAMEPAD % 8)) & 1;
        if (!has_gamepad_btn) { close(fd); continue; }

        // Build per-axis normalisation info
        GamepadDev dev;
        dev.fd             = fd;
        dev.controller_idx = next_idx_++;

        static const int kAxes[] = {
            ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y
        };
        for (int ax : kAxes) {
            struct input_absinfo info{};
            if (ioctl(fd, EVIOCGABS(ax), &info) == 0 && info.maximum != info.minimum) {
                AxisInfo ai;
                ai.minimum    = info.minimum;
                ai.maximum    = info.maximum;
                ai.logical_id = static_cast<uint32_t>(absToLogical(ax));
                dev.axes[ax]  = ai;
            }
        }

        char name[256] = "Unknown";
        (void)ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        std::cout << "[GamepadManager] Found gamepad " << dev.controller_idx
                  << ": " << name << " (" << path << ")\n";

        fds_.push_back(fd);
        devices_.push_back(std::move(dev));
    }
    closedir(dir);
    return static_cast<int>(devices_.size());
}

// ── Event reading ──────────────────────────────────────────────────────────

std::vector<shared::InputMessage> GamepadManager::read(int fd) noexcept {
    std::vector<shared::InputMessage> msgs;

    // Find the device for this fd
    GamepadDev* dev = nullptr;
    for (auto& d : devices_)
        if (d.fd == fd) { dev = &d; break; }
    if (!dev) return msgs;

    struct input_event ev{};
    while (::read(fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
        if (ev.type == EV_KEY) {
            // Only forward gamepad buttons (BTN_GAMEPAD range: 0x130–0x13F)
            if (ev.code < BTN_GAMEPAD || ev.code > BTN_GAMEPAD + 0x0F) continue;
            if (ev.value == 2) continue; // skip key-repeat
            msgs.push_back({
                .type_      = shared::InputEventType::GamepadButton,
                .value_a_   = static_cast<int32_t>(ev.code),
                .value_b_   = ev.value,   // 1=pressed, 0=released
                .modifiers_ = dev->controller_idx,
                .char_value_= 0,
            });
        } else if (ev.type == EV_ABS) {
            auto it = dev->axes.find(static_cast<int>(ev.code));
            if (it == dev->axes.end()) continue;
            const AxisInfo& ai = it->second;
            if (ai.logical_id == static_cast<uint32_t>(-1)) continue;

            // Normalise: min→−1000, max→+1000 (triggers 0→+1000)
            int32_t range = ai.maximum - ai.minimum;
            int32_t norm  = range > 0
                ? (static_cast<int32_t>(ev.value - ai.minimum) * 2000 / range) - 1000
                : 0;
            // Triggers (LT/RT): remap [-1000,+1000] → [0,+1000]
            if (ai.logical_id == 4 || ai.logical_id == 5)
                norm = (norm + 1000) / 2;

            msgs.push_back({
                .type_      = shared::InputEventType::GamepadAxis,
                .value_a_   = static_cast<int32_t>(ai.logical_id),
                .value_b_   = norm,
                .modifiers_ = dev->controller_idx,
                .char_value_= 0,
            });
        }
    }
    return msgs;
}

GamepadManager::~GamepadManager() noexcept {
    for (int fd : fds_) close(fd);
}

} // namespace fbx::xserver
