#include "xserver/wakelock_service.h"
#include "xserver/toggle_store.h"
#include <cstring>
#include <ctime>

namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
// Persist active + absolute end timestamp (0 = indefinite). Derive remaining.
shared::WakeLockState load() {
    shared::WakeLockState st{};
    uint32_t v[2] = {0, 0};   // {active, ends_at_ts}
    toggle::loadInts("wakelock", v, 2);
    uint32_t active = v[0], ends = v[1];
    uint32_t now = static_cast<uint32_t>(time(nullptr));
    if (active && ends != 0 && now >= ends) { active = 0; ends = 0; }  // expired
    st.active_ = active;
    st.remaining_secs_ = (active && ends != 0) ? (ends - now) : 0u;
    return st;
}
void broadcast(const shared::WakeLockState& st) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, "com.freebsdx.WakeLock", sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::WakeLockOpcode::StateChanged);
    std::memcpy(b.payload_, &st, sizeof(st));
    BroadcastXBusMessage(b);
}
}  // namespace

bool WakeLockService::isActive() noexcept { return load().active_ != 0; }

void WakeLockService::handleMessage(const shared::XBusMessage& msg,
                                    std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::WakeLockOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState: {
        shared::WakeLockState st = load();
        shared::XBusMessage m{}; m.transaction_id_ = msg.transaction_id_;
        std::strncpy(m.service_name_, "com.freebsdx.WakeLock", sizeof(m.service_name_) - 1);
        m.opcode_ = static_cast<uint32_t>(O::GetState);
        std::memcpy(m.payload_, &st, sizeof(st));
        client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
        break;
    }
    case O::SetLock: {   // protocol: request → broadcast (no ack)
        shared::WakeLockRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t now = static_cast<uint32_t>(time(nullptr));
        uint32_t active = req.active_ ? 1u : 0u;
        uint32_t ends = (active && req.duration_secs_ > 0) ? (now + req.duration_secs_) : 0u;
        uint32_t v[2] = {active, ends};
        toggle::saveInts("wakelock", v, 2);
        broadcast(load());
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
