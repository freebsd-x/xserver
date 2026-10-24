#include "xserver/gesture_service.h"
#include "xserver/config_service.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include <algorithm>
namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
constexpr const char* kService = "com.freebsdx.Gesture";
uint32_t cfg(const char* k, uint32_t d) {
    std::string v = ConfigService::get(k, std::to_string(d));
    try { return static_cast<uint32_t>(std::stoul(v)); } catch (...) { return d; }
}
shared::GestureConfig build() {
    shared::GestureConfig c{};
    c.enabled_            = cfg("gesture.enabled", 1) ? 1u : 0u;
    c.swipe3_sensitivity_ = std::min(cfg("gesture.swipe3", 50), 100u);
    c.swipe4_sensitivity_ = std::min(cfg("gesture.swipe4", 50), 100u);
    c.pinch_sensitivity_  = std::min(cfg("gesture.pinch", 50), 100u);
    c.tap_to_click_       = cfg("gesture.tap_to_click", 1) ? 1u : 0u;
    c.natural_scroll_     = cfg("gesture.natural_scroll", 0) ? 1u : 0u;
    c.two_finger_scroll_  = cfg("gesture.two_finger_scroll", 1) ? 1u : 0u;
    c.edge_scroll_        = cfg("gesture.edge_scroll", 0) ? 1u : 0u;
    return c;
}
void send(std::shared_ptr<IpcSocket>& cl, uint32_t tid, uint32_t op, const shared::GestureConfig& c) {
    if (!cl) return;
    shared::XBusMessage m{}; m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = op; std::memcpy(m.payload_, &c, sizeof(c));
    cl->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
void bcast(const shared::GestureConfig& c) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::GestureOpcode::ConfigChanged);
    std::memcpy(b.payload_, &c, sizeof(c)); BroadcastXBusMessage(b);
}
}  // namespace
void GestureService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::GestureOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetConfig:
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::GetConfig), build());
        break;
    case O::SetConfig: {
        shared::GestureConfig c{}; std::memcpy(&c, msg.payload_, sizeof(c));
        ConfigService::set("gesture.enabled",           std::to_string(c.enabled_ ? 1u : 0u));
        ConfigService::set("gesture.swipe3",            std::to_string(std::min(c.swipe3_sensitivity_, 100u)));
        ConfigService::set("gesture.swipe4",            std::to_string(std::min(c.swipe4_sensitivity_, 100u)));
        ConfigService::set("gesture.pinch",             std::to_string(std::min(c.pinch_sensitivity_, 100u)));
        ConfigService::set("gesture.tap_to_click",      std::to_string(c.tap_to_click_ ? 1u : 0u));
        ConfigService::set("gesture.natural_scroll",    std::to_string(c.natural_scroll_ ? 1u : 0u));
        ConfigService::set("gesture.two_finger_scroll", std::to_string(c.two_finger_scroll_ ? 1u : 0u));
        ConfigService::set("gesture.edge_scroll",       std::to_string(c.edge_scroll_ ? 1u : 0u));
        auto nc = build(); bcast(nc);
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::SetConfig), nc);
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
