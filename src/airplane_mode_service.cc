#include "xserver/airplane_mode_service.h"
#include "xserver/toggle_store.h"

#include <cstring>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
shared::AirplaneModeState load() {
    shared::AirplaneModeState st{};
    uint32_t v[3] = {0, 0, 0};
    toggle::loadInts("airplane", v, 3);
    st.enabled_ = v[0]; st.wifi_was_on_ = v[1]; st.bt_was_on_ = v[2];
    return st;
}
void reply(IpcSocket* c, uint32_t tid, shared::AirplaneModeOpcode op,
           const shared::AirplaneModeState& st) {
    shared::XBusMessage m{};
    m.transaction_id_ = tid;
    std::strncpy(m.service_name_, "com.freebsdx.AirplaneMode", sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(op);
    std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
}  // namespace

void AirplaneModeService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::AirplaneModeOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState:
        reply(client.get(), msg.transaction_id_, O::GetState, load());
        break;
    case O::SetEnabled: {
        shared::AirplaneModeEnableRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        shared::AirplaneModeState st = load();
        st.enabled_ = req.enabled_ ? 1u : 0u;   // radio control is a follow-up
        uint32_t v[3] = {st.enabled_, st.wifi_was_on_, st.bt_was_on_};
        toggle::saveInts("airplane", v, 3);
        reply(client.get(), msg.transaction_id_, O::SetEnabled, st);

        shared::XBusMessage b{};
        std::strncpy(b.service_name_, "com.freebsdx.AirplaneMode", sizeof(b.service_name_) - 1);
        b.opcode_ = static_cast<uint32_t>(O::StateChanged);
        std::memcpy(b.payload_, &st, sizeof(st));
        BroadcastXBusMessage(b);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
