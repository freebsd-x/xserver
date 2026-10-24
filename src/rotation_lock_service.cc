#include "xserver/rotation_lock_service.h"
#include "xserver/toggle_store.h"
#include <cstring>

namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
shared::RotationLockState load() {
    shared::RotationLockState st{};
    uint32_t v[2] = {0, 0};
    toggle::loadInts("rotationlock", v, 2);
    st.locked_ = v[0]; st.orientation_ = v[1];
    return st;
}
void save(const shared::RotationLockState& st) {
    uint32_t v[2] = {st.locked_, st.orientation_};
    toggle::saveInts("rotationlock", v, 2);
}
void send(IpcSocket* c, uint32_t tid, shared::RotationLockOpcode op,
          const shared::RotationLockState& st) {
    shared::XBusMessage m{}; m.transaction_id_ = tid;
    std::strncpy(m.service_name_, "com.freebsdx.RotationLock", sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(op);
    std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
void broadcast(const shared::RotationLockState& st) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, "com.freebsdx.RotationLock", sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::RotationLockOpcode::StateChanged);
    std::memcpy(b.payload_, &st, sizeof(st));
    BroadcastXBusMessage(b);
}
}  // namespace

void RotationLockService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::RotationLockOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState:
        send(client.get(), msg.transaction_id_, O::GetState, load());
        break;
    case O::SetLocked: {
        shared::RotationLockRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        shared::RotationLockState st = load();
        st.locked_ = req.locked_ ? 1u : 0u;
        save(st);
        send(client.get(), msg.transaction_id_, O::SetLocked, st);   // ack
        broadcast(st);
        break;
    }
    case O::SetOrientation: {
        shared::RotationOrientationRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        shared::RotationLockState st = load();
        st.orientation_ = req.orientation_;
        save(st);
        send(client.get(), msg.transaction_id_, O::SetOrientation, st);   // ack
        broadcast(st);
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
