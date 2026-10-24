#include "xserver/screen_timeout_service.h"
#include "xserver/config_service.h"

#include <cstring>
#include <cstdlib>
#include <string>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
constexpr const char* kService = "com.freebsdx.ScreenTimeout";
constexpr const char* kKey     = "screen.timeout_secs";
constexpr uint32_t    kMax     = 86400;   // 24h; also 0 = never

uint32_t current() {
    std::string v = ConfigService::get(kKey, "300");   // default 5 min
    uint32_t n = 300;
    try { n = static_cast<uint32_t>(std::stoul(v)); } catch (...) { n = 300; }
    return n > kMax ? kMax : n;
}

void sendState(std::shared_ptr<IpcSocket>& client, uint32_t tid, uint32_t opcode, uint32_t secs) {
    if (!client) return;
    shared::ScreenTimeoutState st{}; st.timeout_secs_ = secs;
    shared::XBusMessage m{};
    m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = opcode;
    std::memcpy(m.payload_, &st, sizeof(st));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

void broadcast(uint32_t secs) {
    shared::ScreenTimeoutState st{}; st.timeout_secs_ = secs;
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::ScreenTimeoutOpcode::TimeoutChanged);
    std::memcpy(b.payload_, &st, sizeof(st));
    BroadcastXBusMessage(b);
}
}  // namespace

void ScreenTimeoutService::handleMessage(const shared::XBusMessage& msg,
                                         std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::ScreenTimeoutOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetTimeout:
        sendState(client, msg.transaction_id_, static_cast<uint32_t>(O::GetTimeout), current());
        break;
    case O::SetTimeout: {
        shared::ScreenTimeoutRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t secs = req.timeout_secs_ > kMax ? kMax : req.timeout_secs_;
        ConfigService::set(kKey, std::to_string(secs));
        broadcast(secs);
        sendState(client, msg.transaction_id_, static_cast<uint32_t>(O::SetTimeout), secs);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
