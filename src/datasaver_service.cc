#include "xserver/datasaver_service.h"
#include "xserver/toggle_store.h"

#include <cstring>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
shared::DataSaverState load() {
    shared::DataSaverState st{};
    uint32_t v[1] = {0};
    toggle::loadInts("datasaver", v, 1);
    st.enabled_ = v[0];
    st.saved_mb_today_ = 0;   // display-only estimate; not tracked yet
    return st;
}
void reply(IpcSocket* c, uint32_t tid, shared::DataSaverOpcode op,
           const shared::DataSaverState& st) {
    shared::XBusMessage m{};
    m.transaction_id_ = tid;
    std::strncpy(m.service_name_, "com.freebsdx.DataSaver", sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(op);
    std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
}  // namespace

void DataSaverService::handleMessage(const shared::XBusMessage& msg,
                                     std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::DataSaverOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState:
        reply(client.get(), msg.transaction_id_, O::GetState, load());
        break;
    case O::SetState: {
        shared::DataSaverRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        shared::DataSaverState st = load();
        st.enabled_ = req.enabled_ ? 1u : 0u;
        uint32_t v[1] = {st.enabled_};
        toggle::saveInts("datasaver", v, 1);
        reply(client.get(), msg.transaction_id_, O::SetState, st);

        shared::XBusMessage b{};
        std::strncpy(b.service_name_, "com.freebsdx.DataSaver", sizeof(b.service_name_) - 1);
        b.opcode_ = static_cast<uint32_t>(O::StateChanged);
        std::memcpy(b.payload_, &st, sizeof(st));
        BroadcastXBusMessage(b);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
