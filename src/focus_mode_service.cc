#include "xserver/focus_mode_service.h"
#include "xserver/config_service.h"
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
constexpr const char* kService = "com.freebsdx.FocusMode";
uint32_t cfg(const char* k, uint32_t d) {
    std::string v = ConfigService::get(k, std::to_string(d));
    try { return static_cast<uint32_t>(std::stoul(v)); } catch (...) { return d; }
}
shared::FocusModeState build() {
    shared::FocusModeState st{};
    uint32_t active = cfg("focus.active", 0);
    uint32_t end_ts = cfg("focus.end_ts", 0);           // 0 = unlimited
    uint32_t now    = static_cast<uint32_t>(time(nullptr));
    if (active && end_ts != 0 && now >= end_ts) {       // timed session expired
        active = 0;
        ConfigService::set("focus.active", "0");
        ConfigService::set("focus.end_ts", "0");
    }
    st.active_         = active;
    st.remaining_secs_ = (active && end_ts != 0) ? (end_ts - now) : 0u;
    st.category_       = cfg("focus.category", 2);       // default All
    return st;
}
void bcast(const shared::FocusModeState& st) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::FocusModeOpcode::StateChanged);
    std::memcpy(b.payload_, &st, sizeof(st)); BroadcastXBusMessage(b);
}
}  // namespace
shared::FocusModeState FocusModeService::currentState() noexcept { return build(); }
void FocusModeService::handleMessage(const shared::XBusMessage& msg,
                                     std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::FocusModeOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState: {
        auto st = build();
        if (client) {
            shared::XBusMessage m{}; m.transaction_id_ = msg.transaction_id_;
            std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
            m.opcode_ = static_cast<uint32_t>(O::GetState);
            std::memcpy(m.payload_, &st, sizeof(st));
            client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
        }
        break;
    }
    case O::Start: {
        shared::FocusStartRequest req{}; std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t now = static_cast<uint32_t>(time(nullptr));
        uint32_t end = req.duration_secs_ ? (now + req.duration_secs_) : 0u;
        ConfigService::set("focus.active", "1");
        ConfigService::set("focus.end_ts", std::to_string(end));
        ConfigService::set("focus.category", std::to_string(req.category_ > 2 ? 2u : req.category_));
        bcast(build());
        break;
    }
    case O::Stop:
        ConfigService::set("focus.active", "0");
        ConfigService::set("focus.end_ts", "0");
        bcast(build());
        break;
    default: break;
    }
}
}  // namespace fbx::xserver
