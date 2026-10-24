#include "xserver/backlight_service.h"
#include "xserver/backlight.h"
#include "xserver/async_shell.h"
#include "xserver/ipc_socket.h"
#include <sys/types.h>
#include <sys/sysctl.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>

namespace fbx::xserver {

uint32_t BacklightService::current_level_ = 0xFFu; // uninitialised

// ── Sysctl helpers ───────────────────────────────────────────────────────────

uint32_t BacklightService::getLevel() noexcept {
    // Prefer the backlight(8) device (/dev/backlight0 is group `video`, which the
    // compositor is already in for DRM — no root needed). Fall back to the ACPI
    // video sysctl on machines that only expose that.
    int b = backlight::parseBrightness(spawnCapture("backlight 2>/dev/null"));
    if (b >= 0) { current_level_ = static_cast<uint32_t>(b); return current_level_; }

    int brightness = 80; // safe fallback
    size_t len = sizeof(brightness);
    if (sysctlbyname("hw.acpi.video.lcd0.brightness", &brightness, &len,
                     nullptr, 0) == 0) {
        current_level_ = static_cast<uint32_t>(std::clamp(brightness, 0, 100));
    } else if (current_level_ == 0xFFu) {
        current_level_ = 80; // no backlight device or ACPI video — assume 80%
    }
    return current_level_;
}

void BacklightService::writeLevel(uint32_t level) noexcept {
    level = std::min(level, 100u);
    current_level_ = level;
    // backlight(8) device first (video group, no root); the level is a clamped
    // integer so the command is injection-free. ACPI sysctl fallback.
    if (spawnStatus("backlight " + std::to_string(level) + " >/dev/null 2>&1") == 0) {
        std::cout << "[BacklightService] Brightness set to " << level << "% (backlight device)\n";
        return;
    }
    int val = static_cast<int>(level);
    if (sysctlbyname("hw.acpi.video.lcd0.brightness", nullptr, nullptr,
                     &val, sizeof(val)) == 0) {
        std::cout << "[BacklightService] Brightness set to " << level << "% (ACPI)\n";
    } else {
        std::cerr << "[BacklightService] no backlight device or ACPI video — not applied\n";
    }
}

// ── step() — called directly from key handler in main.cc ─────────────────────

void BacklightService::step(int delta) noexcept {
    uint32_t cur = getLevel();
    int next = static_cast<int>(cur) + delta;
    writeLevel(static_cast<uint32_t>(std::clamp(next, 0, 100)));
}

// ── ProcessMessage ────────────────────────────────────────────────────────────

void BacklightService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                      const shared::XBusMessage& msg) noexcept {
    using namespace shared;
    XBusMessage resp = msg;

    switch (static_cast<BacklightOpcode>(msg.opcode_)) {

    case BacklightOpcode::GetBrightness: {
        BacklightState st{};
        st.level_ = getLevel();
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &st, sizeof(st));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case BacklightOpcode::SetBrightness: {
        BacklightState req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        writeLevel(req.level_);
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        BacklightState st{};
        st.level_ = current_level_;
        std::memcpy(resp.payload_, &st, sizeof(st));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case BacklightOpcode::StepUp: {
        BacklightStepRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t s = (req.step_ > 0 && req.step_ <= 100) ? req.step_ : 10u;
        step(static_cast<int>(s));
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        BacklightState st{};
        st.level_ = current_level_;
        std::memcpy(resp.payload_, &st, sizeof(st));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case BacklightOpcode::StepDown: {
        BacklightStepRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t s = (req.step_ > 0 && req.step_ <= 100) ? req.step_ : 10u;
        step(-static_cast<int>(s));
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        BacklightState st{};
        st.level_ = current_level_;
        std::memcpy(resp.payload_, &st, sizeof(st));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    default:
        std::cerr << "[BacklightService] Unknown opcode: 0x"
                  << std::hex << msg.opcode_ << "\n";
        break;
    }
}

} // namespace fbx::xserver
