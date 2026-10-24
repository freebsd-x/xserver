#include "xserver/system_sounds_service.h"
#include "xserver/config_service.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include <algorithm>
namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
constexpr const char* kService = "com.freebsdx.SystemSounds";
uint32_t cfg(const char* k, uint32_t d) {
    std::string v = ConfigService::get(k, std::to_string(d));
    try { return static_cast<uint32_t>(std::stoul(v)); } catch (...) { return d; }
}
shared::SystemSoundsState current() {
    shared::SystemSoundsState st{};
    st.enabled_    = cfg("sound.enabled", 1) ? 1u : 0u;
    st.volume_pct_ = std::min(cfg("sound.volume_pct", 80), 100u);
    return st;
}
void send(std::shared_ptr<IpcSocket>& c, uint32_t tid, uint32_t op, const shared::SystemSoundsState& st) {
    if (!c) return;
    shared::XBusMessage m{}; m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = op; std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
void bcast(const shared::SystemSoundsState& st) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::SystemSoundsOpcode::StateChanged);
    std::memcpy(b.payload_, &st, sizeof(st)); BroadcastXBusMessage(b);
}
}  // namespace
void SystemSoundsService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::SystemSoundsOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState:
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::GetState), current());
        break;
    case O::SetState: {
        shared::SystemSoundsRequest req{}; std::memcpy(&req, msg.payload_, sizeof(req));
        ConfigService::set("sound.enabled", std::to_string(req.enabled_ ? 1u : 0u));
        ConfigService::set("sound.volume_pct", std::to_string(std::min(req.volume_pct_, 100u)));
        auto st = current(); bcast(st);
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::SetState), st);
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
