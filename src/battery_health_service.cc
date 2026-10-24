#include "xserver/battery_health_service.h"
#include "xserver/battery_health.h"
#include "xserver/config_service.h"
#include "xserver/async_shell.h"

#include <cstring>
#include <cstdlib>
#include <string>
#include <cstdint>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
constexpr const char* kService = "com.freebsdx.BatteryHealth";
constexpr const char* kKey     = "batteryhealth.optimized_charging";

shared::BatteryHealthState build() {
    shared::BatteryHealthState st{};
    // Real wear data (posix_spawn, not popen — async_shell).
    auto b = battery::parseAcpiconf(spawnCapture("acpiconf -i 0 2>/dev/null"));
    st.health_pct_  = battery::healthPct(b);
    st.cycle_count_ = b.cycle_count;
    // Persisted preference (no hardware effect on FreeBSD).
    std::string v = ConfigService::get(kKey, "0");
    try { st.optimized_charging_ = std::stoul(v) ? 1u : 0u; } catch (...) { st.optimized_charging_ = 0u; }
    return st;
}

void send(std::shared_ptr<IpcSocket>& c, uint32_t tid, uint32_t op,
          const shared::BatteryHealthState& st) {
    if (!c) return;
    shared::XBusMessage m{};
    m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = op;
    std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

void bcast(const shared::BatteryHealthState& st) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::BatteryHealthOpcode::StateChanged);
    std::memcpy(b.payload_, &st, sizeof(st));
    BroadcastXBusMessage(b);
}
}  // namespace

void BatteryHealthService::handleMessage(const shared::XBusMessage& msg,
                                         std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::BatteryHealthOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState:
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::GetState), build());
        break;
    case O::SetState: {
        shared::BatteryHealthRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        ConfigService::set(kKey, std::to_string(req.optimized_charging_ ? 1u : 0u));
        auto st = build();
        bcast(st);
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::SetState), st);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
