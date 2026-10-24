#include "xserver/touchpad_service.h"
#include "xserver/config_service.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include <functional>
namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
constexpr const char* kService = "com.freebsdx.Touchpad";
constexpr const char* kKey     = "touchpad.enabled";
std::function<void(bool)> g_apply;   // set via init(); applies to InputManager
uint32_t current() {
    std::string v = ConfigService::get(kKey, "1");
    try { return std::stoul(v) ? 1u : 0u; } catch (...) { return 1u; }
}
void send(std::shared_ptr<IpcSocket>& c, uint32_t tid, uint32_t op, uint32_t en) {
    if (!c) return;
    shared::TouchpadState st{}; st.enabled_ = en;
    shared::XBusMessage m{}; m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = op; std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
void bcast(uint32_t en) {
    shared::TouchpadState st{}; st.enabled_ = en;
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::TouchpadOpcode::StateChanged);
    std::memcpy(b.payload_, &st, sizeof(st)); BroadcastXBusMessage(b);
}
}  // namespace
void TouchpadService::init(std::function<void(bool)> apply) noexcept {
    g_apply = std::move(apply);
    if (g_apply) g_apply(current() != 0);   // push the persisted state on startup
}
void TouchpadService::handleMessage(const shared::XBusMessage& msg,
                                    std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::TouchpadOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState:
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::GetState), current());
        break;
    case O::SetEnabled: {
        shared::TouchpadRequest req{}; std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t en = req.enabled_ ? 1u : 0u;
        ConfigService::set(kKey, std::to_string(en));
        if (g_apply) g_apply(en != 0);   // apply live to the input devices
        bcast(en);
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
