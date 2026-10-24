#include "xserver/refresh_rate_service.h"
#include "xserver/config_service.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include <functional>
namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
constexpr const char* kService = "com.freebsdx.RefreshRate";
constexpr const char* kKey     = "display.refresh_hz";
uint32_t g_supported_mask = 0x1;          // real mask supplied by init()
std::function<bool(uint32_t)> g_apply;    // performs the modeset; returns success

// Bit index for a rate, or -1 if not one of the known steps.
int rateBit(uint32_t hz) {
    switch (hz) { case 60: return 0; case 120: return 1; case 144: return 2; case 240: return 3; }
    return -1;
}
bool supported(uint32_t hz) {
    int b = rateBit(hz);
    return b >= 0 && (g_supported_mask & (1u << b));
}
uint32_t currentHz() {
    std::string v = ConfigService::get(kKey, "60");
    uint32_t hz = 60;
    try { hz = static_cast<uint32_t>(std::stoul(v)); } catch (...) { hz = 60; }
    return supported(hz) ? hz : 60u;
}
shared::RefreshRateState build() {
    shared::RefreshRateState st{};
    st.current_hz_     = currentHz();
    st.supported_mask_ = g_supported_mask;
    return st;
}
void send(std::shared_ptr<IpcSocket>& c, uint32_t tid, uint32_t op, const shared::RefreshRateState& st) {
    if (!c) return;
    shared::XBusMessage m{}; m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = op; std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
void bcast(const shared::RefreshRateState& st) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::RefreshRateOpcode::RateChanged);
    std::memcpy(b.payload_, &st, sizeof(st)); BroadcastXBusMessage(b);
}
}  // namespace
void RefreshRateService::init(uint32_t supported_mask, std::function<bool(uint32_t)> apply) noexcept {
    g_supported_mask = supported_mask ? supported_mask : 0x1u;
    g_apply = std::move(apply);
    // Re-apply the persisted rate if it is supported and differs from the current.
    uint32_t hz = currentHz();
    if (g_apply && hz != 60) g_apply(hz);
}
void RefreshRateService::handleMessage(const shared::XBusMessage& msg,
                                       std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::RefreshRateOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetRate:
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::GetRate), build());
        break;
    case O::SetRate: {
        shared::RefreshRateRequest req{}; std::memcpy(&req, msg.payload_, sizeof(req));
        if (supported(req.hz_)) {
            // Perform the modeset (no hook in tests → treat as accepted); persist +
            // broadcast only when it actually took.
            bool ok = g_apply ? g_apply(req.hz_) : true;
            if (ok) {
                ConfigService::set(kKey, std::to_string(req.hz_));
                bcast(build());
            }
        }
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::SetRate), build());
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
