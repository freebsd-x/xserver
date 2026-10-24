#include "xserver/spatial_audio_service.h"
#include "xserver/config_service.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include <algorithm>
namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
constexpr const char* kService = "com.freebsdx.SpatialAudio";
uint32_t cfg(const char* k, uint32_t d) {
    std::string v = ConfigService::get(k, std::to_string(d));
    try { return static_cast<uint32_t>(std::stoul(v)); } catch (...) { return d; }
}
shared::SpatialAudioConfig build() {
    shared::SpatialAudioConfig c{};
    c.enabled_ = cfg("spatialaudio.enabled", 0) ? 1u : 0u;
    c.mode_    = std::min(cfg("spatialaudio.mode", 0), 2u);
    std::strncpy(c.device_, "Default Output", sizeof(c.device_) - 1);
    return c;
}
void send(std::shared_ptr<IpcSocket>& cl, uint32_t tid, uint32_t op, const shared::SpatialAudioConfig& c) {
    if (!cl) return;
    shared::XBusMessage m{}; m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = op; std::memcpy(m.payload_, &c, sizeof(c));
    cl->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
void bcast(const shared::SpatialAudioConfig& c) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::SpatialAudioOpcode::ConfigChanged);
    std::memcpy(b.payload_, &c, sizeof(c)); BroadcastXBusMessage(b);
}
}  // namespace
void SpatialAudioService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::SpatialAudioOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetConfig:
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::GetConfig), build());
        break;
    case O::SetConfig: {
        shared::SpatialAudioConfig c{}; std::memcpy(&c, msg.payload_, sizeof(c));
        ConfigService::set("spatialaudio.enabled", std::to_string(c.enabled_ ? 1u : 0u));
        ConfigService::set("spatialaudio.mode", std::to_string(std::min(c.mode_, 2u)));
        auto nc = build(); bcast(nc);
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::SetConfig), nc);
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
