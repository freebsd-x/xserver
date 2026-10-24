#include "xserver/nightlight_service.h"
#include "xserver/toggle_store.h"

#include <cstring>
#include <fstream>
#include <string>
#include <array>
#include <algorithm>
#include <functional>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
std::function<void(const std::array<double, 9>&)> g_apply;   // tint hook (set via init)
shared::NightLightState load() {
    shared::NightLightState st{};
    st.enabled_ = 0;
    st.temperature_k_ = 4000;      // sensible default (warm)
    st.schedule_enabled_ = 0;
    std::strncpy(st.from_time_, "22:00", sizeof(st.from_time_) - 1);
    std::strncpy(st.to_time_,   "06:00", sizeof(st.to_time_) - 1);

    std::ifstream f(toggle::path("nightlight"));
    if (f.is_open()) {
        std::string from, to;
        f >> st.enabled_ >> st.temperature_k_ >> st.schedule_enabled_ >> from >> to;
        if (!from.empty()) std::strncpy(st.from_time_, from.c_str(), sizeof(st.from_time_) - 1);
        if (!to.empty())   std::strncpy(st.to_time_,   to.c_str(),   sizeof(st.to_time_) - 1);
    }
    return st;
}
void save(const shared::NightLightState& st) {
    std::ofstream f(toggle::path("nightlight"));
    if (f.is_open())
        f << st.enabled_ << ' ' << st.temperature_k_ << ' ' << st.schedule_enabled_
          << ' ' << st.from_time_ << ' ' << st.to_time_ << '\n';
}
void broadcast(const shared::NightLightState& st) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, "com.freebsdx.NightLight", sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::NightLightOpcode::StateChanged);
    std::memcpy(b.payload_, &st, sizeof(st));
    BroadcastXBusMessage(b);
}

// Push the current tint matrix to the installed hook (if any).
void pushTint(const shared::NightLightState& st) {
    if (g_apply) g_apply(NightLightService::computeMatrix(st));
}
}  // namespace

void NightLightService::handleMessage(const shared::XBusMessage& msg,
                                      std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::NightLightOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState: {
        shared::NightLightState st = load();
        shared::XBusMessage m{};
        m.transaction_id_ = msg.transaction_id_;
        std::strncpy(m.service_name_, "com.freebsdx.NightLight", sizeof(m.service_name_) - 1);
        m.opcode_ = static_cast<uint32_t>(O::GetState);
        std::memcpy(m.payload_, &st, sizeof(st));
        client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
        break;
    }
    case O::SetEnabled: {   // no response — persist + broadcast
        shared::NightLightEnableRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        shared::NightLightState st = load();
        st.enabled_ = req.enabled_ ? 1u : 0u;
        save(st); pushTint(st); broadcast(st);
        break;
    }
    case O::SetTemperature: {
        shared::NightLightTempRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        shared::NightLightState st = load();
        if (req.temperature_k_ >= 2700 && req.temperature_k_ <= 6500)
            st.temperature_k_ = req.temperature_k_;
        save(st); pushTint(st); broadcast(st);
        break;
    }
    case O::SetSchedule: {
        shared::NightLightScheduleRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.from_time_[sizeof(req.from_time_) - 1] = '\0';
        req.to_time_[sizeof(req.to_time_) - 1]     = '\0';
        shared::NightLightState st = load();
        st.schedule_enabled_ = req.enabled_ ? 1u : 0u;
        std::strncpy(st.from_time_, req.from_time_, sizeof(st.from_time_) - 1);
        std::strncpy(st.to_time_,   req.to_time_,   sizeof(st.to_time_) - 1);
        save(st); broadcast(st);
        break;
    }
    default: break;
    }
}

std::array<double, 9> NightLightService::computeMatrix(const shared::NightLightState& st) noexcept {
    if (!st.enabled_) return {1,0,0, 0,1,0, 0,0,1};
    // Warm tint: leave red at full, pull green/blue down as the temperature drops.
    // 6500K ≈ neutral (1,1,1); 2700K ≈ warmest (1, 0.78, 0.54). Linear in Kelvin.
    double k = std::clamp(static_cast<double>(st.temperature_k_), 2700.0, 6500.0);
    double frac = (k - 2700.0) / (6500.0 - 2700.0);        // 0 at 2700K, 1 at 6500K
    double g = 0.78 + frac * (1.0 - 0.78);
    double b = 0.54 + frac * (1.0 - 0.54);
    return {1.0, 0.0, 0.0,
            0.0, g,   0.0,
            0.0, 0.0, b};
}

void NightLightService::init(std::function<void(const std::array<double, 9>&)> apply) noexcept {
    g_apply = std::move(apply);
    if (g_apply) g_apply(computeMatrix(load()));   // push current at startup
}

}  // namespace fbx::xserver
