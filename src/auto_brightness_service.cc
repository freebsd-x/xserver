#include "xserver/auto_brightness_service.h"
#include "xserver/config_service.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include <cstdint>
namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
constexpr const char* kService = "com.freebsdx.AutoBrightness";
constexpr const char* kKey     = "autobrightness.enabled";
shared::AutoBrightnessState build() {
    shared::AutoBrightnessState st{};
    std::string v = ConfigService::get(kKey, "0");
    try { st.enabled_ = std::stoul(v) ? 1u : 0u; } catch (...) { st.enabled_ = 0u; }
    st.lux_ = UINT32_MAX;   // no ambient-light sensor
    return st;
}
void send(std::shared_ptr<IpcSocket>& c, uint32_t tid, uint32_t op, const shared::AutoBrightnessState& st) {
    if (!c) return;
    shared::XBusMessage m{}; m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = op; std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
void bcast(const shared::AutoBrightnessState& st) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::AutoBrightnessOpcode::StateChanged);
    std::memcpy(b.payload_, &st, sizeof(st)); BroadcastXBusMessage(b);
}
}  // namespace
void AutoBrightnessService::handleMessage(const shared::XBusMessage& msg,
                                          std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::AutoBrightnessOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState:
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::GetState), build());
        break;
    case O::SetEnabled: {
        shared::AutoBrightnessRequest req{}; std::memcpy(&req, msg.payload_, sizeof(req));
        ConfigService::set(kKey, std::to_string(req.enabled_ ? 1u : 0u));
        auto st = build(); bcast(st);
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::SetEnabled), st);
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
