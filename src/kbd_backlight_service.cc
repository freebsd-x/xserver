#include "xserver/kbd_backlight_service.h"
#include "xserver/config_service.h"
#include "shared/logger.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
constexpr const char* kSvc = "com.freebsdx.KeyboardBacklight";

uint32_t cfgUint(const char* key, uint32_t dflt) {
    try { return static_cast<uint32_t>(std::stoul(ConfigService::get(key, std::to_string(dflt)))); }
    catch (...) { return dflt; }
}

shared::KeyboardBacklightState build() {
    shared::KeyboardBacklightState st{};
    st.enabled_    = cfgUint("kbdbl.enabled", 0) ? 1u : 0u;
    st.brightness_ = std::min(cfgUint("kbdbl.brightness", 50), 100u);
    return st;
}

void ackAndBroadcast(std::shared_ptr<IpcSocket>& c, const shared::XBusMessage& req, uint32_t op) {
    auto st = build();
    if (c) {
        shared::XBusMessage r{};
        r.transaction_id_ = req.transaction_id_;
        std::strncpy(r.service_name_, kSvc, sizeof(r.service_name_) - 1);
        r.opcode_ = op;
        std::memcpy(r.payload_, &st, sizeof(st));
        c->sendMessage({reinterpret_cast<const uint8_t*>(&r), sizeof(r)});
    }
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kSvc, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::KeyboardBacklightOpcode::StateChanged);
    std::memcpy(b.payload_, &st, sizeof(st));
    BroadcastXBusMessage(b);
}
}  // namespace

void KbdBacklightService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::KeyboardBacklightOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState: {
        auto st = build();
        if (client) {
            shared::XBusMessage r{};
            r.transaction_id_ = msg.transaction_id_;
            std::strncpy(r.service_name_, kSvc, sizeof(r.service_name_) - 1);
            r.opcode_ = static_cast<uint32_t>(O::GetState);
            std::memcpy(r.payload_, &st, sizeof(st));
            client->sendMessage({reinterpret_cast<const uint8_t*>(&r), sizeof(r)});
        }
        break;
    }
    case O::SetEnabled: {
        shared::KeyboardBacklightEnableRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        ConfigService::set("kbdbl.enabled", req.enabled_ ? "1" : "0");
        ackAndBroadcast(client, msg, static_cast<uint32_t>(O::SetEnabled));
        break;
    }
    case O::SetBrightness: {
        shared::KeyboardBacklightBrightnessRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t b = std::min(req.brightness_, 100u);   // clamp 0–100
        ConfigService::set("kbdbl.brightness", std::to_string(b));
        ackAndBroadcast(client, msg, static_cast<uint32_t>(O::SetBrightness));
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
