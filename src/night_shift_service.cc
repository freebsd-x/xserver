#include "xserver/night_shift_service.h"
#include "xserver/ipc_socket.h"
#include "xserver/weather_service.h"
#include <cstring>
#include <ctime>
#include <iostream>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

std::atomic<uint32_t> NightShiftService::enabled_  {0};
std::atomic<uint32_t> NightShiftService::intensity_ {50};
std::atomic<uint32_t> NightShiftService::auto_mode_ {0};

void NightShiftService::broadcastState() noexcept {
    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.NightShift",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::NightShiftOpcode::StateChanged);
    shared::NightShiftState st{};
    st.enabled_   = enabled_.load();
    st.intensity_ = intensity_.load();
    st.auto_mode_ = auto_mode_.load();
    std::memcpy(bcast.payload_, &st, sizeof(st));
    BroadcastXBusMessage(bcast);
}

// Simple sunrise/sunset estimate: enable if local hour is outside [7, 20).
// A real implementation would use WeatherService lat/lon + sun angle math.
void NightShiftService::pollAutoMode() noexcept {
    if (!auto_mode_.load()) return;
    std::time_t now = std::time(nullptr);
    std::tm* lt = std::localtime(&now);
    if (!lt) return;
    bool should_be_on = (lt->tm_hour < 7 || lt->tm_hour >= 20);
    uint32_t cur = enabled_.load();
    if (should_be_on && !cur) {
        enabled_.store(1);
        std::cout << "[NightShift] Auto-enabled (hour=" << lt->tm_hour << ")\n";
        broadcastState();
    } else if (!should_be_on && cur) {
        enabled_.store(0);
        std::cout << "[NightShift] Auto-disabled (hour=" << lt->tm_hour << ")\n";
        broadcastState();
    }
}

bool NightShiftService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                       const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.NightShift") != 0) return false;

    using Op = shared::NightShiftOpcode;
    shared::XBusMessage response = msg;

    switch (static_cast<Op>(msg.opcode_)) {

    case Op::GetState: {
        shared::NightShiftState st{};
        st.enabled_   = enabled_.load();
        st.intensity_ = intensity_.load();
        st.auto_mode_ = auto_mode_.load();
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case Op::SetConfig: {
        shared::NightShiftConfig cfg{};
        std::memcpy(&cfg, msg.payload_, sizeof(cfg));
        enabled_.store(cfg.enabled_);
        intensity_.store(std::min(cfg.intensity_, 100u));
        auto_mode_.store(cfg.auto_mode_);
        std::cout << "[NightShift] Config: enabled=" << cfg.enabled_
                  << " intensity=" << cfg.intensity_
                  << " auto=" << cfg.auto_mode_ << "\n";
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        broadcastState();
        break;
    }

    default:
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    return true;
}

} // namespace fbx::xserver
