#include "xserver/pointer_service.h"
#include "xserver/config_service.h"

#include <cstring>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <functional>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
constexpr const char* kService = "com.freebsdx.Pointer";
std::function<void(uint32_t, bool, bool)> g_apply;   // set via init()

uint32_t cfgU32(const char* key, uint32_t def) {
    std::string v = ConfigService::get(key, std::to_string(def));
    try { return static_cast<uint32_t>(std::stoul(v)); } catch (...) { return def; }
}

shared::PointerConfig currentConfig() {
    shared::PointerConfig c{};
    c.speed_          = std::clamp(cfgU32("pointer.speed", 5), 1u, 10u);
    c.natural_scroll_ = cfgU32("pointer.natural_scroll", 0) ? 1u : 0u;
    c.acceleration_   = cfgU32("pointer.acceleration", 1) ? 1u : 0u;
    c.tap_to_click_   = cfgU32("pointer.tap_to_click", 1) ? 1u : 0u;
    return c;
}

void sendConfig(std::shared_ptr<IpcSocket>& client, uint32_t tid, uint32_t opcode,
                const shared::PointerConfig& c) {
    if (!client) return;
    shared::XBusMessage m{};
    m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = opcode;
    std::memcpy(m.payload_, &c, sizeof(c));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

void broadcast(const shared::PointerConfig& c) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::PointerOpcode::ConfigChanged);
    std::memcpy(b.payload_, &c, sizeof(c));
    BroadcastXBusMessage(b);
}
}  // namespace

void PointerService::init(std::function<void(uint32_t, bool, bool)> apply) noexcept {
    g_apply = std::move(apply);
    if (g_apply) {
        auto c = currentConfig();
        g_apply(c.speed_, c.acceleration_ != 0, c.tap_to_click_ != 0);
    }
}

void PointerService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::PointerOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetConfig:
        sendConfig(client, msg.transaction_id_, static_cast<uint32_t>(O::GetConfig), currentConfig());
        break;
    case O::SetConfig: {
        shared::PointerConfig c{};
        std::memcpy(&c, msg.payload_, sizeof(c));
        c.speed_          = std::clamp(c.speed_, 1u, 10u);
        c.natural_scroll_ = c.natural_scroll_ ? 1u : 0u;
        c.acceleration_   = c.acceleration_ ? 1u : 0u;
        c.tap_to_click_   = c.tap_to_click_ ? 1u : 0u;
        ConfigService::set("pointer.speed",          std::to_string(c.speed_));
        ConfigService::set("pointer.natural_scroll", std::to_string(c.natural_scroll_));
        ConfigService::set("pointer.acceleration",   std::to_string(c.acceleration_));
        ConfigService::set("pointer.tap_to_click",   std::to_string(c.tap_to_click_));
        if (g_apply) g_apply(c.speed_, c.acceleration_ != 0, c.tap_to_click_ != 0);  // live
        broadcast(c);
        sendConfig(client, msg.transaction_id_, static_cast<uint32_t>(O::SetConfig), c);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
