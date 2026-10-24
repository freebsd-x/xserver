#include "xserver/hardware_service.h"
#include "xserver/ipc_socket.h"
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/soundcard.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <cstring>

namespace fbx::xserver {

// Read master volume (0-100) from /dev/mixer. Returns -1 on failure.
static int readVolume() noexcept {
    int fd = open("/dev/mixer", O_RDWR);
    if (fd < 0) return -1;
    int vol = 0;
    if (ioctl(fd, SOUND_MIXER_READ_VOLUME, &vol) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    // vol encodes left channel in low byte, right in high byte; average them.
    int left  = vol & 0xFF;
    int right = (vol >> 8) & 0xFF;
    return (left + right) / 2;
}

// Write master volume (0-100) to /dev/mixer.
static void writeVolume(uint32_t level) noexcept {
    if (level > 100) level = 100;
    int fd = open("/dev/mixer", O_RDWR);
    if (fd < 0) {
        std::cerr << "[HardwareService] open(/dev/mixer) failed\n";
        return;
    }
    int vol = static_cast<int>(level) | (static_cast<int>(level) << 8);
    if (ioctl(fd, SOUND_MIXER_WRITE_VOLUME, &vol) < 0) {
        std::cerr << "[HardwareService] SOUND_MIXER_WRITE failed\n";
    } else {
        std::cout << "[HardwareService] Volume set to: " << level << "%\n";
    }
    close(fd);
}

bool HardwareService::ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.Hardware") != 0) return false;

    shared::XBusMessage response = msg;
    shared::HardwareStatus status = getStatus();

    if (msg.opcode_ == static_cast<uint32_t>(shared::HardwareOpcode::GetBattery)) {
        std::memcpy(response.payload_, &status, sizeof(status));
    } else if (msg.opcode_ == static_cast<uint32_t>(shared::HardwareOpcode::GetBacklight)) {
        std::memcpy(response.payload_, &status, sizeof(status));
    } else if (msg.opcode_ == static_cast<uint32_t>(shared::HardwareOpcode::SetBacklight)) {
        // Callers send a HardwareStatus struct; extract backlight_level_ from it.
        shared::HardwareStatus req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t level = req.backlight_level_;
        if (level > 100) level = 100;
        int val = static_cast<int>(level);
        if (sysctlbyname("hw.acpi.video.lcd0.brightness", nullptr, nullptr, &val, sizeof(val)) == 0) {
            std::cout << "[HardwareService] Backlight set to: " << level << "%\n";
        } else {
            std::cerr << "[HardwareService] sysctlbyname for backlight failed (no ACPI video support?)\n";
        }
    } else if (msg.opcode_ == static_cast<uint32_t>(shared::HardwareOpcode::GetVolume)) {
        int vol = readVolume();
        if (vol >= 0) status.volume_level_ = static_cast<uint32_t>(vol);
        std::memcpy(response.payload_, &status, sizeof(status));
    } else if (msg.opcode_ == static_cast<uint32_t>(shared::HardwareOpcode::SetVolume)) {
        // Callers send a HardwareStatus struct; extract volume_level_ from it.
        shared::HardwareStatus req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        writeVolume(req.volume_level_);
    }

    std::span<const uint8_t> resp_span(reinterpret_cast<const uint8_t*>(&response), sizeof(response));
    (void)client->sendMessage(resp_span);
    return true;
}

shared::HardwareStatus HardwareService::getStatus() noexcept {
    shared::HardwareStatus status{100, 1, 80, 50}; // Defaults

    // 1. Query Battery Life via FreeBSD sysctl
    int life = 0;
    size_t len = sizeof(life);
    if (sysctlbyname("hw.acpi.battery.life", &life, &len, nullptr, 0) == 0) {
        status.battery_life_ = static_cast<uint32_t>(life);
    }

    // 2. Query Battery State (0=discharging, 1=charging)
    int state = 0;
    len = sizeof(state);
    if (sysctlbyname("hw.acpi.battery.state", &state, &len, nullptr, 0) == 0) {
        status.battery_state_ = static_cast<uint32_t>(state);
    }

    // 3. Query Backlight (Mocked if hardware not present)
    int brightness = 80;
    len = sizeof(brightness);
    if (sysctlbyname("hw.acpi.video.lcd0.brightness", &brightness, &len, nullptr, 0) == 0) {
        status.backlight_level_ = static_cast<uint32_t>(brightness);
    }

    // 4. Query volume from /dev/mixer
    int vol = readVolume();
    if (vol >= 0) status.volume_level_ = static_cast<uint32_t>(vol);

    return status;
}

} // namespace fbx::xserver
