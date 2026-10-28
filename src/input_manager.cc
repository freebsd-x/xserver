#include "xserver/input_manager.h"
#include <iostream>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <unistd.h>

namespace fbx::xserver {

// Translate a Linux keycode to its US-QWERTY printable ASCII character.
// Returns 0 for non-printable keys (modifiers, function keys, etc.).
static int32_t translateKey(uint32_t key, bool shift) noexcept {
    struct KV { char n, s; };
    // Indexed directly by Linux keycode
    static constexpr KV table[] = {
        {0,0},       // 0: reserved
        {0,0},       // 1: Esc
        {'1','!'},   // 2
        {'2','@'},   // 3
        {'3','#'},   // 4
        {'4','$'},   // 5
        {'5','%'},   // 6
        {'6','^'},   // 7
        {'7','&'},   // 8
        {'8','*'},   // 9
        {'9','('},   // 10
        {'0',')'},   // 11
        {'-','_'},   // 12
        {'=','+'},   // 13
        {0,0},       // 14: Backspace
        {'\t','\t'}, // 15: Tab
        {'q','Q'},   // 16
        {'w','W'},   // 17
        {'e','E'},   // 18
        {'r','R'},   // 19
        {'t','T'},   // 20
        {'y','Y'},   // 21
        {'u','U'},   // 22
        {'i','I'},   // 23
        {'o','O'},   // 24
        {'p','P'},   // 25
        {'[','{'},   // 26
        {']','}'},   // 27
        {'\n','\n'}, // 28: Enter
        {0,0},       // 29: Left Ctrl
        {'a','A'},   // 30
        {'s','S'},   // 31
        {'d','D'},   // 32
        {'f','F'},   // 33
        {'g','G'},   // 34
        {'h','H'},   // 35
        {'j','J'},   // 36
        {'k','K'},   // 37
        {'l','L'},   // 38
        {';',':'},   // 39
        {'\'','"'},  // 40
        {'`','~'},   // 41
        {0,0},       // 42: Left Shift
        {'\\','|'},  // 43
        {'z','Z'},   // 44
        {'x','X'},   // 45
        {'c','C'},   // 46
        {'v','V'},   // 47
        {'b','B'},   // 48
        {'n','N'},   // 49
        {'m','M'},   // 50
        {',','<'},   // 51
        {'.','>'},   // 52
        {'/','?'},   // 53
        {0,0},       // 54: Right Shift
        {'*','*'},   // 55: Keypad *
        {0,0},       // 56: Left Alt
        {' ',' '},   // 57: Space
    };
    static constexpr size_t kTableSize = sizeof(table) / sizeof(table[0]);
    if (key >= kTableSize) return 0;
    char c = shift ? table[key].s : table[key].n;
    return static_cast<int32_t>(static_cast<unsigned char>(c));
}

void UdevDeleter::operator()(udev* u) const noexcept {
    if (u) udev_unref(u);
}

void LibinputDeleter::operator()(libinput* li) const noexcept {
    if (li) libinput_unref(li);
}

static const struct libinput_interface interface = {
    .open_restricted = InputManager::openRestricted,
    .close_restricted = InputManager::closeRestricted,
};

InputManager::InputManager(ISeatManager* seatManager) noexcept
    : seat_manager_(seatManager) {}

std::expected<std::unique_ptr<InputManager>, std::error_code> InputManager::create(ISeatManager* seatManager) noexcept {
    auto manager = std::unique_ptr<InputManager>(new InputManager(seatManager));
    if (auto res = manager->init(); !res.has_value()) {
        return std::unexpected(res.error());
    }
    return manager;
}

std::expected<void, std::error_code> InputManager::init() noexcept {
    udev_.reset(udev_new());
    if (!udev_) return std::unexpected(std::make_error_code(std::errc::not_enough_memory));

    libinput_.reset(libinput_udev_create_context(&interface, this, udev_.get()));
    if (!libinput_) return std::unexpected(std::make_error_code(std::errc::not_supported));

    if (libinput_udev_assign_seat(libinput_.get(), "seat0") != 0) {
        return std::unexpected(std::make_error_code(std::errc::permission_denied));
    }
    
    std::cout << "[input_manager] Successfully initialized libinput via seatd.\n";
    return {};
}

int InputManager::getFd() const noexcept {
    return libinput_get_fd(libinput_.get());
}

// Per-device config applied at add time: tap-to-click (libinput defaults it off,
// which makes laptop touchpads feel unclickable) + the current natural-scroll
// direction.
bool InputManager::isTouchpad(libinput_device* dev) noexcept {
    return libinput_device_config_tap_get_finger_count(dev) > 0;
}

// Apply the current pointer prefs (accel speed/profile + tap-to-click) to a device.
void InputManager::applyPointerTo(libinput_device* dev) noexcept {
    if (libinput_device_config_accel_is_available(dev)) {
        auto ov = device_overrides_.find(inputdev::deviceId(libinput_device_get_name(dev)));
        if (ov != device_overrides_.end()) {
            libinput_device_config_accel_set_speed(dev, inputdev::sensitivityToSpeed(ov->second.sensitivity));
            libinput_device_config_accel_set_profile(dev,
                ov->second.adaptive ? LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE
                                    : LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT);
        } else {
            // Map speed step 1..10 → libinput's [-1.0, +1.0].
            uint32_t s = pointer_speed_ < 1 ? 1 : (pointer_speed_ > 10 ? 10 : pointer_speed_);
            libinput_device_config_accel_set_speed(dev, (static_cast<double>(s) - 1.0) / 9.0 * 2.0 - 1.0);
            libinput_device_config_accel_set_profile(dev,
                pointer_accel_ ? LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE
                               : LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT);
        }
    }
    if (isTouchpad(dev))
        libinput_device_config_tap_set_enabled(dev,
            pointer_tap_ ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
}

void InputManager::applyDeviceConfig(libinput_device* dev) noexcept {
    if (isTouchpad(dev)) {
        libinput_device_config_tap_set_drag_enabled(dev, LIBINPUT_CONFIG_DRAG_ENABLED);
        // Honour the persisted touchpad enable/disable for hot-plugged devices.
        libinput_device_config_send_events_set_mode(dev,
            touchpad_enabled_ ? LIBINPUT_CONFIG_SEND_EVENTS_ENABLED
                              : LIBINPUT_CONFIG_SEND_EVENTS_DISABLED);
    }
    applyPointerTo(dev);   // accel + tap-to-click (per current pointer prefs)
    if (libinput_device_config_scroll_has_natural_scroll(dev))
        libinput_device_config_scroll_set_natural_scroll_enabled(dev, natural_scroll_ ? 1 : 0);
}

std::vector<inputdev::DeviceInfo> InputManager::listDevices() const noexcept {
    std::vector<inputdev::DeviceInfo> out;
    for (libinput_device* d : devices_) {
        inputdev::DeviceInfo i;
        i.name    = libinput_device_get_name(d);
        i.id      = inputdev::deviceId(i.name);
        i.vendor  = libinput_device_get_id_vendor(d);
        i.product = libinput_device_get_id_product(d);
        i.type    = inputdev::pickType(
            libinput_device_has_capability(d, LIBINPUT_DEVICE_CAP_KEYBOARD),
            libinput_device_has_capability(d, LIBINPUT_DEVICE_CAP_POINTER),
            libinput_device_has_capability(d, LIBINPUT_DEVICE_CAP_TOUCH),
            libinput_device_has_capability(d, LIBINPUT_DEVICE_CAP_TABLET_TOOL));
        if (libinput_device_config_accel_is_available(d)) {
            i.sensitivity = inputdev::speedToSensitivity(libinput_device_config_accel_get_speed(d));
            i.adaptive    = libinput_device_config_accel_get_profile(d) != LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
        }
        out.push_back(std::move(i));
    }
    return out;
}

void InputManager::setDeviceOverride(uint32_t id, uint32_t sensitivity, bool adaptive) noexcept {
    device_overrides_[id] = {sensitivity > 100 ? 100u : sensitivity, adaptive};
    for (libinput_device* d : devices_)
        if (inputdev::deviceId(libinput_device_get_name(d)) == id) applyPointerTo(d);
}

void InputManager::setPointerConfig(uint32_t speed, bool acceleration, bool tap_to_click) noexcept {
    pointer_speed_ = speed; pointer_accel_ = acceleration; pointer_tap_ = tap_to_click;
    for (libinput_device* d : devices_) applyPointerTo(d);
    std::cout << "[xserver] Pointer config: speed=" << speed
              << " accel=" << acceleration << " tap=" << tap_to_click << "\n";
}

void InputManager::setNaturalScroll(bool enabled) noexcept {
    natural_scroll_ = enabled;
    for (libinput_device* d : devices_)
        if (libinput_device_config_scroll_has_natural_scroll(d))
            libinput_device_config_scroll_set_natural_scroll_enabled(d, enabled ? 1 : 0);
    std::cout << "[xserver] Natural scroll " << (enabled ? "on" : "off") << "\n";
}

void InputManager::setTouchpadEnabled(bool enabled) noexcept {
    touchpad_enabled_ = enabled;
    for (libinput_device* d : devices_)
        if (isTouchpad(d))
            libinput_device_config_send_events_set_mode(d,
                enabled ? LIBINPUT_CONFIG_SEND_EVENTS_ENABLED
                        : LIBINPUT_CONFIG_SEND_EVENTS_DISABLED);
    std::cout << "[xserver] Touchpad " << (enabled ? "enabled" : "disabled") << "\n";
}

std::expected<std::vector<shared::InputMessage>, std::error_code> InputManager::dispatch() noexcept {
    if (libinput_dispatch(libinput_.get()) != 0) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    std::vector<shared::InputMessage> messages;
    libinput_event* event;

    while ((event = libinput_get_event(libinput_.get())) != nullptr) {
        auto type = libinput_event_get_type(event);

        if (type == LIBINPUT_EVENT_DEVICE_ADDED) {
            libinput_device* dev = libinput_event_get_device(event);
            std::cout << "[xserver] Device Added: " << libinput_device_get_name(dev) << "\n";
            // Retain the device so natural-scroll changes can be re-applied live.
            libinput_device_ref(dev);
            devices_.push_back(dev);
            applyDeviceConfig(dev);
            if (on_devices_changed_) on_devices_changed_();
        } else if (type == LIBINPUT_EVENT_DEVICE_REMOVED) {
            libinput_device* dev = libinput_event_get_device(event);
            std::cout << "[xserver] Device Removed: " << libinput_device_get_name(dev) << "\n";
            auto it = std::find(devices_.begin(), devices_.end(), dev);
            if (it != devices_.end()) { libinput_device_unref(*it); devices_.erase(it); }
            if (on_devices_changed_) on_devices_changed_();
        } else if (type == LIBINPUT_EVENT_POINTER_MOTION) {
            auto* motion = libinput_event_get_pointer_event(event);
            messages.push_back({
                .type_ = shared::InputEventType::MouseMove,
                .value_a_ = static_cast<int32_t>(libinput_event_pointer_get_dx(motion)),
                .value_b_ = static_cast<int32_t>(libinput_event_pointer_get_dy(motion)),
                .modifiers_ = modifiers_
            });
        } else if (type == LIBINPUT_EVENT_POINTER_BUTTON) {
            auto* button = libinput_event_get_pointer_event(event);
            messages.push_back({
                .type_ = shared::InputEventType::MouseButton,
                .value_a_ = static_cast<int32_t>(libinput_event_pointer_get_button(button)),
                .value_b_ = static_cast<int32_t>(libinput_event_pointer_get_button_state(button)),
                .modifiers_ = modifiers_
            });
        } else if (type == LIBINPUT_EVENT_KEYBOARD_KEY) {
            auto* key_event = libinput_event_get_keyboard_event(event);
            uint32_t key = libinput_event_keyboard_get_key(key_event);
            auto state = libinput_event_keyboard_get_key_state(key_event);

            // Update global modifier state
            // Keycodes: Shift(42,54), Ctrl(29,97), Alt(56,100), Super(125,126)
            bool pressed = (state == LIBINPUT_KEY_STATE_PRESSED);
            if (key == 42 || key == 54) { if (pressed) modifiers_ |= 1; else modifiers_ &= ~1; }
            else if (key == 29 || key == 97) { if (pressed) modifiers_ |= 2; else modifiers_ &= ~2; }
            else if (key == 56 || key == 100) { if (pressed) modifiers_ |= 4; else modifiers_ &= ~4; }
            else if (key == 125 || key == 126) { if (pressed) modifiers_ |= 8; else modifiers_ &= ~8; }

            // EMERGENCY EXIT: Ctrl+Alt+Esc
            if (key == 1 && state == LIBINPUT_KEY_STATE_PRESSED
                && (modifiers_ & 2) && (modifiers_ & 4)) {
                std::cout << "[input_manager] Emergency shutdown (Ctrl+Alt+Esc). Sending SIGTERM...\n";
                kill(getpid(), SIGTERM);
            }

            // DASHBOARD TOGGLE: Super + C (Keycode 46)
            if (key == 46 && state == LIBINPUT_KEY_STATE_PRESSED && (modifiers_ & 8)) {
                std::cout << "[input_manager] DASHBOARD TOGGLE REQUESTED (Super+C).\n";
                messages.push_back({
                    .type_ = shared::InputEventType::DashboardToggle,
                    .value_a_ = 0,
                    .value_b_ = 0,
                    .modifiers_ = modifiers_
                });
            }

            // COLOR PICKER TOGGLE: Super + P (Keycode 25)
            if (key == 25 && state == LIBINPUT_KEY_STATE_PRESSED && (modifiers_ & 8)) {
                std::cout << "[input_manager] COLOR PICKER TOGGLE (Super+P).\n";
                messages.push_back({
                    .type_ = shared::InputEventType::ColorPickerToggle,
                    .value_a_ = 0, .value_b_ = 0, .modifiers_ = modifiers_
                });
            }

            // CLIPBOARD HISTORY TOGGLE: Super + V (Keycode 47)
            if (key == 47 && state == LIBINPUT_KEY_STATE_PRESSED && (modifiers_ & 8)) {
                std::cout << "[input_manager] CLIPBOARD HISTORY TOGGLE (Super+V).\n";
                messages.push_back({
                    .type_ = shared::InputEventType::ClipboardHistoryToggle,
                    .value_a_ = 0,
                    .value_b_ = 0,
                    .modifiers_ = modifiers_
                });
            }

            // SEARCH TOGGLE: Super + Space (Keycode 57)
            if (key == 57 && state == LIBINPUT_KEY_STATE_PRESSED && (modifiers_ & 8)) {
                std::cout << "[input_manager] SEARCH TOGGLE REQUESTED (Super+Space).\n";
                messages.push_back({
                    .type_ = shared::InputEventType::SearchToggle,
                    .value_a_ = 0,
                    .value_b_ = 0,
                    .modifiers_ = modifiers_
                });
            }

            // WORKSPACE SWITCH: Super + 1..9 (Keycodes 2..10)
            if (key >= 2 && key <= 10 && state == LIBINPUT_KEY_STATE_PRESSED && (modifiers_ & 8)) {
                int ws_index = key - 2;
                std::cout << "[input_manager] WORKSPACE SWITCH: " << (ws_index + 1) << "\n";
                messages.push_back({
                    .type_ = shared::InputEventType::WorkspaceSwitch,
                    .value_a_ = ws_index,
                    .value_b_ = 0,
                    .modifiers_ = modifiers_
                });
            }

            // ALT+TAB / ALT+SHIFT+TAB: cycle window focus (Tab keycode = 15)
            if (key == 15 && state == LIBINPUT_KEY_STATE_PRESSED && (modifiers_ & 4)) {
                int direction = (modifiers_ & 1) ? -1 : 1; // Shift = backward
                messages.push_back({
                    .type_      = shared::InputEventType::CycleFocus,
                    .value_a_   = 0,
                    .value_b_   = direction,
                    .modifiers_ = modifiers_
                });
            }

            // ALT RELEASE: dismiss the switcher overlay (detect before modifier is cleared)
            if ((key == 56 || key == 100) && state == LIBINPUT_KEY_STATE_RELEASED) {
                messages.push_back({
                    .type_      = shared::InputEventType::SwitcherDismiss,
                    .value_a_   = 0,
                    .value_b_   = 0,
                    .modifiers_ = modifiers_
                });
            }

            // MOVE WINDOW TO ADJACENT OUTPUT: Super + Shift + Left/Right (Keycodes 105, 106)
            if ((key == 105 || key == 106) && state == LIBINPUT_KEY_STATE_PRESSED
                    && (modifiers_ & 8) && (modifiers_ & 1)) {
                int direction = (key == 105) ? -1 : 1;
                std::cout << "[input_manager] MOVE WINDOW TO OUTPUT (Super+Shift+"
                          << (direction < 0 ? "Left" : "Right") << ")\n";
                messages.push_back({
                    .type_      = shared::InputEventType::MoveWindowToOutput,
                    .value_a_   = 0,
                    .value_b_   = direction,
                    .modifiers_ = modifiers_
                });
            }

            // WORKSPACE SLIDE: Super + Left/Right (no Shift) (Keycodes 105, 106)
            if ((key == 105 || key == 106) && state == LIBINPUT_KEY_STATE_PRESSED
                    && (modifiers_ & 8) && !(modifiers_ & 1)) {
                int direction = (key == 105) ? -1 : 1;
                messages.push_back({
                    .type_      = shared::InputEventType::WorkspaceSwitch,
                    .value_a_   = -1, // -1 means relative slide
                    .value_b_   = direction,
                    .modifiers_ = modifiers_
                });
            }

            // SNAP MAXIMIZE: Super + Up (Keycode 103, no Shift)
            if (key == 103 && state == LIBINPUT_KEY_STATE_PRESSED
                    && (modifiers_ & 8) && !(modifiers_ & 1)) {
                messages.push_back({
                    .type_      = shared::InputEventType::SnapWindow,
                    .value_a_   = 0, // maximize
                    .value_b_   = 0,
                    .modifiers_ = modifiers_
                });
            }

            // SNAP RESTORE: Super + Down (Keycode 108, no Shift)
            if (key == 108 && state == LIBINPUT_KEY_STATE_PRESSED
                    && (modifiers_ & 8) && !(modifiers_ & 1)) {
                messages.push_back({
                    .type_      = shared::InputEventType::SnapWindow,
                    .value_a_   = 2, // restore
                    .value_b_   = 0,
                    .modifiers_ = modifiers_
                });
            }

            // SNAP LEFT: Super + Left (Keycode 105)
            if (key == 105 && state == LIBINPUT_KEY_STATE_PRESSED
                    && (modifiers_ & 8) && !(modifiers_ & 1)) {
                messages.push_back({
                    .type_      = shared::InputEventType::SnapWindow,
                    .value_a_   = 1, // snap left
                    .value_b_   = 0,
                    .modifiers_ = modifiers_
                });
            }

            // SNAP RIGHT: Super + Right (Keycode 106)
            if (key == 106 && state == LIBINPUT_KEY_STATE_PRESSED
                    && (modifiers_ & 8) && !(modifiers_ & 1)) {
                messages.push_back({
                    .type_      = shared::InputEventType::SnapWindow,
                    .value_a_   = 3, // snap right
                    .value_b_   = 0,
                    .modifiers_ = modifiers_
                });
            }

            // QUARTER-SNAP: Super + Shift + arrow  (value_a_: 4=TL 5=TR 6=BL 7=BR)
            if (state == LIBINPUT_KEY_STATE_PRESSED
                    && (modifiers_ & 8) && (modifiers_ & 1)) {  // Super + Shift
                int qa = -1;
                if      (key == 105) qa = 4; // Left  → top-left
                else if (key == 106) qa = 5; // Right → top-right
                else if (key == 108) qa = 6; // Down  → bottom-left
                else if (key == 103) qa = 7; // Up    → bottom-right
                // Cycle: if already in that corner, advance clockwise TL→TR→BR→BL→TL
                if (qa >= 0) {
                    messages.push_back({
                        .type_      = shared::InputEventType::SnapWindow,
                        .value_a_   = qa,
                        .value_b_   = 0,
                        .modifiers_ = modifiers_
                    });
                }
            }

            // LAYOUT CYCLE: Super + A (Keycode 30)
            if (key == 30 && state == LIBINPUT_KEY_STATE_PRESSED && (modifiers_ & 8)) {
                std::cout << "[input_manager] LAYOUT CYCLE REQUESTED (Super+A).\n";
                messages.push_back({
                    .type_ = shared::InputEventType::WorkspaceSwitch,
                    .value_a_ = -2, // -2 means cycle layout
                    .value_b_ = 0,
                    .modifiers_ = modifiers_
                });
            }

            // MASTER RATIO: Super+H shrink (35), Super+L grow (38) — tiling mode.
            // (Super WITHOUT Shift; Super+Shift+H/L is directional focus below.)
            if ((key == 35 || key == 38) && state == LIBINPUT_KEY_STATE_PRESSED &&
                (modifiers_ & 8) && !(modifiers_ & 1)) {
                messages.push_back({
                    .type_      = shared::InputEventType::WorkspaceSwitch,
                    .value_a_   = -3,               // -3 = adjust master ratio
                    .value_b_   = (key == 38) ? 1 : -1, // +1 grow, -1 shrink
                    .modifiers_ = modifiers_
                });
            }

            // DIRECTIONAL FOCUS: Super+Shift+H/J/K/L (vim: 35/36/37/38 = L/D/U/R).
            if ((modifiers_ & 8) && (modifiers_ & 1) &&
                state == LIBINPUT_KEY_STATE_PRESSED &&
                (key == 35 || key == 36 || key == 37 || key == 38)) {
                int dir = key == 35 ? 0 : key == 36 ? 3 : key == 37 ? 2 : 1; // L/D/U/R
                messages.push_back({
                    .type_      = shared::InputEventType::WorkspaceSwitch,
                    .value_a_   = -4,               // -4 = directional focus
                    .value_b_   = dir,              // 0=Left,1=Right,2=Up,3=Down
                    .modifiers_ = modifiers_
                });
            }

            // SWAP WITH MASTER: Super+M (keycode 50) — tiling "zoom".
            if (key == 50 && state == LIBINPUT_KEY_STATE_PRESSED && (modifiers_ & 8)) {
                messages.push_back({
                    .type_      = shared::InputEventType::WorkspaceSwitch,
                    .value_a_   = -5,               // -5 = swap focused with master
                    .value_b_   = 0,
                    .modifiers_ = modifiers_
                });
            }

            // DEBUG OVERLAY TOGGLE: Super + F3 (Keycode 61)
            if (key == 61 && state == LIBINPUT_KEY_STATE_PRESSED && (modifiers_ & 8)) {
                std::cout << "[input_manager] DEBUG OVERLAY TOGGLE (Super+F3).\n";
                messages.push_back({
                    .type_      = shared::InputEventType::DebugOverlayToggle,
                    .value_a_   = 0,
                    .value_b_   = 0,
                    .modifiers_ = modifiers_
                });
            }

            // SHORTCUT HELP TOGGLE: Super + / (Keycode 53)
            if (key == 53 && state == LIBINPUT_KEY_STATE_PRESSED && (modifiers_ & 8)) {
                std::cout << "[input_manager] SHORTCUT HELP TOGGLE (Super+/).\n";
                messages.push_back({
                    .type_      = shared::InputEventType::ShortcutHelpToggle,
                    .value_a_   = 0,
                    .value_b_   = 0,
                    .modifiers_ = modifiers_
                });
            }

            // QUICK NOTE TOGGLE: Super + N (Keycode 49)
            if (key == 49 && state == LIBINPUT_KEY_STATE_PRESSED && (modifiers_ & 8)) {
                std::cout << "[input_manager] QUICK NOTE TOGGLE (Super+N).\n";
                messages.push_back({
                    .type_      = shared::InputEventType::QuickNoteToggle,
                    .value_a_   = 0,
                    .value_b_   = 0,
                    .modifiers_ = modifiers_
                });
            }

            // KEY_BRIGHTNESSDOWN (224) / KEY_BRIGHTNESSUP (225) — hardware
            // brightness keys: emit BrightnessDown/Up and skip generic KeyPress.
            if ((key == 224 || key == 225) && state == LIBINPUT_KEY_STATE_PRESSED) {
                messages.push_back({
                    .type_      = (key == 225) ? shared::InputEventType::BrightnessUp
                                               : shared::InputEventType::BrightnessDown,
                    .value_a_   = 10, // default step: 10 %
                    .value_b_   = 0,
                    .modifiers_ = 0
                });
                libinput_event_destroy(event);
                continue;
            }

            // KEY_SYSRQ (99) = Print Screen — screenshot shortcuts.
            // Plain Print = full-screen capture; Shift+Print = region selector.
            if (key == 99 && state == LIBINPUT_KEY_STATE_PRESSED) {
                bool shifted = (modifiers_ & 1) != 0;
                messages.push_back({
                    .type_      = shifted
                                  ? shared::InputEventType::ScreenshotCaptureRegion
                                  : shared::InputEventType::ScreenshotCapture,
                    .value_a_   = 0, // output_idx = primary
                    .value_b_   = 0,
                    .modifiers_ = 0
                });
                libinput_event_destroy(event);
                continue;
            }

            // XF86AudioPlay(164) / AudioStop(166) / AudioNext(163) / AudioPrev(165)
            // playback media keys: emit MediaPlay/Stop/Next/Prev and skip generic KeyPress.
            if ((key == 164 || key == 166 || key == 163 || key == 165) &&
                state == LIBINPUT_KEY_STATE_PRESSED) {
                shared::InputEventType t =
                    (key == 164) ? shared::InputEventType::MediaPlay :
                    (key == 166) ? shared::InputEventType::MediaStop :
                    (key == 163) ? shared::InputEventType::MediaNext
                                 : shared::InputEventType::MediaPrev;
                messages.push_back({
                    .type_      = t,
                    .value_a_   = 0,
                    .value_b_   = 0,
                    .modifiers_ = 0
                });
                libinput_event_destroy(event);
                continue;
            }

            // KEY_MUTE (113) / KEY_VOLUMEDOWN (114) / KEY_VOLUMEUP (115) — audio
            // media keys: emit AudioMute/VolumeDown/VolumeUp and skip generic KeyPress.
            if ((key == 113 || key == 114 || key == 115) && state == LIBINPUT_KEY_STATE_PRESSED) {
                shared::InputEventType t =
                    (key == 113) ? shared::InputEventType::AudioMute :
                    (key == 115) ? shared::InputEventType::AudioVolumeUp
                                 : shared::InputEventType::AudioVolumeDown;
                messages.push_back({
                    .type_      = t,
                    .value_a_   = 10, // default step: 10 %
                    .value_b_   = 0,
                    .modifiers_ = 0
                });
                libinput_event_destroy(event);
                continue;
            }

            {
                bool shifted = (modifiers_ & 1) != 0;
                int32_t ch = (state == LIBINPUT_KEY_STATE_PRESSED) ? translateKey(key, shifted) : 0;
                messages.push_back({
                    .type_ = shared::InputEventType::KeyPress,
                    .value_a_ = static_cast<int32_t>(key),
                    .value_b_ = static_cast<int32_t>(state),
                    .modifiers_ = modifiers_,
                    .char_value_ = ch
                });
            }
        } else if (type == LIBINPUT_EVENT_POINTER_AXIS) {
            auto* ptr = libinput_event_get_pointer_event(event);
            if (libinput_event_pointer_has_axis(ptr, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) {
                double val = libinput_event_pointer_get_axis_value(ptr, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
                // libinput normalizes discrete scroll to ~3.0 per notch; pass as integer lines
                int32_t lines = static_cast<int32_t>(val);
                if (lines == 0 && val != 0.0) lines = (val > 0.0) ? 1 : -1;
                messages.push_back({
                    .type_ = shared::InputEventType::MouseWheel,
                    .value_a_ = 0,
                    .value_b_ = lines,
                    .modifiers_ = modifiers_,
                    .char_value_ = 0
                });
            }
        } else if (type == LIBINPUT_EVENT_TOUCH_DOWN) {
            auto* touch = libinput_event_get_touch_event(event);
            int slot = libinput_event_touch_get_seat_slot(touch);
            float ax = static_cast<float>(
                libinput_event_touch_get_x_transformed(touch, screen_w_));
            float ay = static_cast<float>(
                libinput_event_touch_get_y_transformed(touch, screen_h_));

            // Proper touch event for gesture-aware widgets.
            messages.push_back({
                .type_      = shared::InputEventType::TouchDown,
                .value_a_   = static_cast<int32_t>(ax),
                .value_b_   = static_cast<int32_t>(ay),
                .modifiers_ = static_cast<uint32_t>(slot),
            });

            // Slot 0: also synthesize mouse-compat events so existing widgets work.
            if (slot == 0) {
                touch0_prev_x_ = ax;
                touch0_prev_y_ = ay;
                messages.push_back({
                    .type_      = shared::InputEventType::MouseButton,
                    .value_a_   = 272, // BTN_LEFT
                    .value_b_   = 1,   // pressed
                    .modifiers_ = modifiers_,
                });
            }

        } else if (type == LIBINPUT_EVENT_TOUCH_UP) {
            auto* touch = libinput_event_get_touch_event(event);
            int slot = libinput_event_touch_get_seat_slot(touch);

            messages.push_back({
                .type_      = shared::InputEventType::TouchUp,
                .value_a_   = 0,
                .value_b_   = 0,
                .modifiers_ = static_cast<uint32_t>(slot),
            });

            if (slot == 0) {
                touch0_prev_x_ = -1.0f;
                touch0_prev_y_ = -1.0f;
                messages.push_back({
                    .type_      = shared::InputEventType::MouseButton,
                    .value_a_   = 272, // BTN_LEFT
                    .value_b_   = 0,   // released
                    .modifiers_ = modifiers_,
                });
            }

        } else if (type == LIBINPUT_EVENT_TOUCH_MOTION) {
            auto* touch = libinput_event_get_touch_event(event);
            int slot = libinput_event_touch_get_seat_slot(touch);
            float ax = static_cast<float>(
                libinput_event_touch_get_x_transformed(touch, screen_w_));
            float ay = static_cast<float>(
                libinput_event_touch_get_y_transformed(touch, screen_h_));

            messages.push_back({
                .type_      = shared::InputEventType::TouchMove,
                .value_a_   = static_cast<int32_t>(ax),
                .value_b_   = static_cast<int32_t>(ay),
                .modifiers_ = static_cast<uint32_t>(slot),
            });

            // Slot 0: emit relative delta as MouseMove for existing scroll/drag widgets.
            if (slot == 0 && touch0_prev_x_ >= 0.0f) {
                messages.push_back({
                    .type_      = shared::InputEventType::MouseMove,
                    .value_a_   = static_cast<int32_t>(ax - touch0_prev_x_),
                    .value_b_   = static_cast<int32_t>(ay - touch0_prev_y_),
                    .modifiers_ = modifiers_,
                });
                touch0_prev_x_ = ax;
                touch0_prev_y_ = ay;
            }

        // ── Stylus / tablet tool ──────────────────────────────────────────────
        } else if (type == LIBINPUT_EVENT_TABLET_TOOL_TIP) {
            auto* tab = libinput_event_get_tablet_tool_event(event);
            float ax  = static_cast<float>(
                libinput_event_tablet_tool_get_x_transformed(tab, screen_w_));
            float ay  = static_cast<float>(
                libinput_event_tablet_tool_get_y_transformed(tab, screen_h_));
            float prs = static_cast<float>(
                libinput_event_tablet_tool_get_pressure(tab)); // 0.0–1.0
            float tx  = static_cast<float>(
                libinput_event_tablet_tool_get_tilt_x(tab));   // degrees
            float ty  = static_cast<float>(
                libinput_event_tablet_tool_get_tilt_y(tab));
            auto tip  = libinput_event_tablet_tool_get_tip_state(tab);
            int16_t packed_tx = static_cast<int16_t>(tx * 10.0f);
            int16_t packed_ty = static_cast<int16_t>(ty * 10.0f);

            messages.push_back({
                .type_       = (tip == LIBINPUT_TABLET_TOOL_TIP_DOWN)
                               ? shared::InputEventType::StylusDown
                               : shared::InputEventType::StylusUp,
                .value_a_    = static_cast<int32_t>(ax),
                .value_b_    = static_cast<int32_t>(ay),
                .modifiers_  = static_cast<uint32_t>(prs * 65535.0f),
                .char_value_ = (static_cast<int32_t>(static_cast<uint16_t>(packed_tx)) << 16) |
                               static_cast<uint16_t>(packed_ty),
            });

        } else if (type == LIBINPUT_EVENT_TABLET_TOOL_AXIS) {
            auto* tab = libinput_event_get_tablet_tool_event(event);
            float ax  = static_cast<float>(
                libinput_event_tablet_tool_get_x_transformed(tab, screen_w_));
            float ay  = static_cast<float>(
                libinput_event_tablet_tool_get_y_transformed(tab, screen_h_));
            float prs = static_cast<float>(
                libinput_event_tablet_tool_get_pressure(tab));
            float tx  = static_cast<float>(
                libinput_event_tablet_tool_get_tilt_x(tab));
            float ty  = static_cast<float>(
                libinput_event_tablet_tool_get_tilt_y(tab));
            int16_t packed_tx = static_cast<int16_t>(tx * 10.0f);
            int16_t packed_ty = static_cast<int16_t>(ty * 10.0f);

            messages.push_back({
                .type_       = shared::InputEventType::StylusMove,
                .value_a_    = static_cast<int32_t>(ax),
                .value_b_    = static_cast<int32_t>(ay),
                .modifiers_  = static_cast<uint32_t>(prs * 65535.0f),
                .char_value_ = (static_cast<int32_t>(static_cast<uint16_t>(packed_tx)) << 16) |
                               static_cast<uint16_t>(packed_ty),
            });

        } else if (type == LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY) {
            auto* tab   = libinput_event_get_tablet_tool_event(event);
            auto  prox  = libinput_event_tablet_tool_get_proximity_state(tab);
            auto* tool  = libinput_event_tablet_tool_get_tool(tab);
            auto  ttype = libinput_tablet_tool_get_type(tool);
            uint32_t tool_idx = 0;
            if (ttype == LIBINPUT_TABLET_TOOL_TYPE_ERASER) tool_idx = 1;
            else if (ttype == LIBINPUT_TABLET_TOOL_TYPE_BRUSH) tool_idx = 2;

            messages.push_back({
                .type_       = shared::InputEventType::StylusProximity,
                .value_a_    = (prox == LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_IN) ? 1 : 0,
                .value_b_    = 0,
                .modifiers_  = tool_idx,
                .char_value_ = 0,
            });

        // ── Gesture: swipe ────────────────────────────────────────────────────
        } else if (type == LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN) {
            messages.push_back({ .type_ = shared::InputEventType::GestureSwipeBegin });
        } else if (type == LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE) {
            auto* swipe = libinput_event_get_gesture_event(event);
            messages.push_back({
                .type_ = shared::InputEventType::GestureSwipeUpdate,
                .value_a_ = static_cast<int32_t>(libinput_event_gesture_get_dx(swipe)),
                .value_b_ = static_cast<int32_t>(libinput_event_gesture_get_dy(swipe))
            });
        } else if (type == LIBINPUT_EVENT_GESTURE_SWIPE_END) {
            messages.push_back({ .type_ = shared::InputEventType::GestureSwipeEnd });

        // ── Gesture: pinch ────────────────────────────────────────────────────
        } else if (type == LIBINPUT_EVENT_GESTURE_PINCH_BEGIN) {
            auto* g = libinput_event_get_gesture_event(event);
            messages.push_back({
                .type_      = shared::InputEventType::GesturePinchBegin,
                .value_a_   = libinput_event_gesture_get_finger_count(g),
                .value_b_   = 0,
                .modifiers_ = 1000, // scale * 1000: 1000 = 1.0× (no change yet)
            });
        } else if (type == LIBINPUT_EVENT_GESTURE_PINCH_UPDATE) {
            auto* g = libinput_event_get_gesture_event(event);
            float scale = static_cast<float>(libinput_event_gesture_get_scale(g));
            messages.push_back({
                .type_      = shared::InputEventType::GesturePinchUpdate,
                .value_a_   = static_cast<int32_t>(libinput_event_gesture_get_dx(g) * 100.0),
                .value_b_   = static_cast<int32_t>(libinput_event_gesture_get_dy(g) * 100.0),
                .modifiers_ = static_cast<uint32_t>(scale * 1000.0f),
                .char_value_= libinput_event_gesture_get_finger_count(g),
            });
        } else if (type == LIBINPUT_EVENT_GESTURE_PINCH_END) {
            auto* g = libinput_event_get_gesture_event(event);
            messages.push_back({
                .type_      = shared::InputEventType::GesturePinchEnd,
                .value_a_   = libinput_event_gesture_get_cancelled(g) ? 1 : 0,
                .value_b_   = 0,
                .modifiers_ = 0,
            });
        }

        libinput_event_destroy(event);
    }

    return messages;
}

int InputManager::openRestricted(const char* path, int /*flags*/, void* userData) noexcept {
    auto* self = static_cast<InputManager*>(userData);
    if (!self || !self->seat_manager_) return -1;
    
    auto res = self->seat_manager_->openDevice(path);
    if (!res) {
        std::cerr << "[input_manager] libseat failed to open " << path << ": " << res.error().message() << "\n";
        return -1;
    }
    
    SeatDevice sd = std::move(res.value());
    int raw_fd = sd.fd_.get();
    
    self->open_devices_.emplace(raw_fd, std::move(sd));
    
    return raw_fd;
}

void InputManager::closeRestricted(int fd, void* userData) noexcept {
    auto* self = static_cast<InputManager*>(userData);
    if (!self || !self->seat_manager_) return;

    auto it = self->open_devices_.find(fd);
    if (it != self->open_devices_.end()) {
        // Release ownership from UniqueFd as libseat_close_device will close it.
        (void)it->second.fd_.release();
        (void)self->seat_manager_->closeDevice(it->second.device_id_);
        self->open_devices_.erase(it);
    }
}

} // namespace fbx::xserver
