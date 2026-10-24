#include "xserver/reduce_motion_service.h"
#include "xserver/toggle_store.h"
#include <cstring>

namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
uint32_t loadEnabled() { uint32_t v[1] = {0}; toggle::loadInts("reducemotion", v, 1); return v[0]; }
void sendState(IpcSocket* c, uint32_t tid, shared::ReduceMotionOpcode op, uint32_t enabled) {
    shared::ReduceMotionState st{}; st.enabled_ = enabled;
    shared::XBusMessage m{}; m.transaction_id_ = tid;
    std::strncpy(m.service_name_, "com.freebsdx.ReduceMotion", sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(op);
    std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
void broadcast(uint32_t enabled) {
    shared::ReduceMotionState st{}; st.enabled_ = enabled;
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, "com.freebsdx.ReduceMotion", sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::ReduceMotionOpcode::StateChanged);
    std::memcpy(b.payload_, &st, sizeof(st));
    BroadcastXBusMessage(b);
}
}  // namespace

void ReduceMotionService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::ReduceMotionOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState:
        sendState(client.get(), msg.transaction_id_, O::GetState, loadEnabled());
        break;
    case O::SetEnabled: {
        shared::ReduceMotionRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t en = req.enabled_ ? 1u : 0u;
        toggle::saveInts("reducemotion", &en, 1);
        broadcast(en);   // no ack (protocol: request → broadcast)
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
