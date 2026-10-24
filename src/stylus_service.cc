#include "xserver/stylus_service.h"
#include "xserver/config_service.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include <algorithm>
namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
constexpr const char* kService = "com.freebsdx.Stylus";
uint32_t cfg(const char* k, uint32_t d) {
    std::string v = ConfigService::get(k, std::to_string(d));
    try { return static_cast<uint32_t>(std::stoul(v)); } catch (...) { return d; }
}
shared::StylusState build() {
    shared::StylusState s{};
    s.connected_      = 0;   // no active-digitiser detection yet
    s.palm_rejection_ = cfg("stylus.palm_rejection", 1) ? 1u : 0u;
    s.pressure_curve_ = std::min(cfg("stylus.pressure_curve", 1), 2u);
    return s;
}
void send(std::shared_ptr<IpcSocket>& c, uint32_t tid, uint32_t op, const shared::StylusState& s) {
    if (!c) return;
    shared::XBusMessage m{}; m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = op; std::memcpy(m.payload_, &s, sizeof(s));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
void bcast(const shared::StylusState& s) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::StylusOpcode::StateChanged);
    std::memcpy(b.payload_, &s, sizeof(s)); BroadcastXBusMessage(b);
}
}  // namespace
void StylusService::handleMessage(const shared::XBusMessage& msg,
                                  std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::StylusOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState:
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::GetState), build());
        break;
    case O::SetPalmReject: {
        shared::StylusRequest r{}; std::memcpy(&r, msg.payload_, sizeof(r));
        ConfigService::set("stylus.palm_rejection", std::to_string(r.palm_rejection_ ? 1u : 0u));
        bcast(build());
        break;
    }
    case O::SetPressure: {
        shared::StylusRequest r{}; std::memcpy(&r, msg.payload_, sizeof(r));
        ConfigService::set("stylus.pressure_curve", std::to_string(std::min(r.pressure_curve_, 2u)));
        bcast(build());
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
