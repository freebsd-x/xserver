#include "xserver/scrolldir_service.h"
#include "xserver/toggle_store.h"
#include <cstring>

namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
uint32_t loadNatural() { uint32_t v[1] = {0}; toggle::loadInts("scrolldir", v, 1); return v[0]; }
std::function<void(bool)> g_apply;   // wired to InputManager::setNaturalScroll
void sendState(IpcSocket* c, uint32_t tid, shared::ScrollDirOpcode op, uint32_t natural) {
    shared::ScrollDirState st{}; st.natural_ = natural;
    shared::XBusMessage m{}; m.transaction_id_ = tid;
    std::strncpy(m.service_name_, "com.freebsdx.ScrollDir", sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(op);
    std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
}  // namespace

void ScrollDirService::init(std::function<void(bool)> apply) noexcept {
    g_apply = std::move(apply);
    if (g_apply) g_apply(loadNatural() != 0);   // apply the persisted setting at startup
}

void ScrollDirService::handleMessage(const shared::XBusMessage& msg,
                                     std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::ScrollDirOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetDir:
        sendState(client.get(), msg.transaction_id_, O::GetDir, loadNatural());
        break;
    case O::SetNatural: {
        shared::ScrollDirRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t nat = req.natural_ ? 1u : 0u;
        toggle::saveInts("scrolldir", &nat, 1);
        if (g_apply) g_apply(nat != 0);   // apply to input devices immediately
        sendState(client.get(), msg.transaction_id_, O::SetNatural, nat);   // ack
        shared::ScrollDirState st{}; st.natural_ = nat;
        shared::XBusMessage b{};
        std::strncpy(b.service_name_, "com.freebsdx.ScrollDir", sizeof(b.service_name_) - 1);
        b.opcode_ = static_cast<uint32_t>(O::DirChanged);
        std::memcpy(b.payload_, &st, sizeof(st));
        BroadcastXBusMessage(b);
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
