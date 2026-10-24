#include "xserver/privacy_service.h"
#include "xserver/config_service.h"
#include <cstring>
#include <cstdlib>
#include <string>
namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
constexpr const char* kService = "com.freebsdx.Privacy";
bool cfgBool(const char* k) {
    std::string v = ConfigService::get(k, "0");
    try { return std::stoul(v) != 0; } catch (...) { return false; }
}
shared::PrivacyStatus current() {
    shared::PrivacyStatus st{};
    st.camera_blocked_ = cfgBool("privacy.camera_blocked") ? 1u : 0u;
    st.mic_blocked_    = cfgBool("privacy.mic_blocked") ? 1u : 0u;
    // camera_app_/mic_app_ left empty: no live device-usage tracking yet.
    return st;
}
void reply(std::shared_ptr<IpcSocket>& c, uint32_t tid, uint32_t op, const shared::PrivacyStatus& st) {
    if (!c) return;
    shared::XBusMessage m{}; m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = op; std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
void bcast(const shared::PrivacyStatus& st) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::PrivacyOpcode::DeviceStateChanged);
    std::memcpy(b.payload_, &st, sizeof(st)); BroadcastXBusMessage(b);
}
}  // namespace
bool PrivacyService::cameraBlocked() noexcept { return cfgBool("privacy.camera_blocked"); }
bool PrivacyService::micBlocked() noexcept    { return cfgBool("privacy.mic_blocked"); }
void PrivacyService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::PrivacyOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetStatus:
        reply(client, msg.transaction_id_, static_cast<uint32_t>(O::GetStatus), current());
        break;
    case O::SetCameraBlocked: {
        shared::PrivacyDeviceRequest req{}; std::memcpy(&req, msg.payload_, sizeof(req));
        ConfigService::set("privacy.camera_blocked", std::to_string(req.blocked_ ? 1u : 0u));
        auto st = current(); bcast(st);
        reply(client, msg.transaction_id_, static_cast<uint32_t>(O::SetCameraBlocked), st);
        break;
    }
    case O::SetMicBlocked: {
        shared::PrivacyDeviceRequest req{}; std::memcpy(&req, msg.payload_, sizeof(req));
        ConfigService::set("privacy.mic_blocked", std::to_string(req.blocked_ ? 1u : 0u));
        auto st = current(); bcast(st);
        reply(client, msg.transaction_id_, static_cast<uint32_t>(O::SetMicBlocked), st);
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
