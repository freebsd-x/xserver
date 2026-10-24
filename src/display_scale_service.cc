#include "xserver/display_scale_service.h"
#include "xserver/config_service.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include <functional>
namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
constexpr const char* kService = "com.freebsdx.DisplayScale";
constexpr const char* kKey     = "display.scale_pct";
std::function<void(uint32_t)> g_apply;   // set via init()
uint32_t validate(uint32_t p) {
    return (p == 100 || p == 150 || p == 200 || p == 300) ? p : 100u;
}
uint32_t current() {
    std::string v = ConfigService::get(kKey, "100");
    try { return validate(static_cast<uint32_t>(std::stoul(v))); } catch (...) { return 100u; }
}
void send(std::shared_ptr<IpcSocket>& c, uint32_t tid, uint32_t op, uint32_t pct) {
    if (!c) return;
    shared::DisplayScaleState st{}; st.scale_pct_ = pct;
    shared::XBusMessage m{}; m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = op; std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
void bcast(uint32_t pct) {
    shared::DisplayScaleState st{}; st.scale_pct_ = pct;
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::DisplayScaleOpcode::ScaleChanged);
    std::memcpy(b.payload_, &st, sizeof(st)); BroadcastXBusMessage(b);
}
}  // namespace
void DisplayScaleService::init(std::function<void(uint32_t)> apply) noexcept {
    g_apply = std::move(apply);
    if (g_apply) g_apply(current());   // push persisted scale at startup
}
void DisplayScaleService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::DisplayScaleOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetScale:
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::GetScale), current());
        break;
    case O::SetScale: {
        shared::DisplayScalePctRequest req{}; std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t pct = validate(req.scale_pct_);
        ConfigService::set(kKey, std::to_string(pct));
        if (g_apply) g_apply(pct);   // apply live to the output
        bcast(pct);
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
