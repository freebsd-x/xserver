#include "xserver/dnd_service.h"
#include "xserver/toggle_store.h"
#include "xserver/dnd_time.h"

#include <cstring>
#include <ctime>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
shared::DoNotDisturbState load() {
    shared::DoNotDisturbState st{};
    uint32_t v[3] = {0, 0, 0};
    toggle::loadInts("dnd", v, 3);
    st.enabled_ = v[0]; st.duration_ = v[1]; st.ends_at_ts_ = v[2];
    // If a timed DND has expired since it was set, report it off.
    if (st.enabled_ && st.ends_at_ts_ != 0 &&
        static_cast<uint32_t>(time(nullptr)) >= st.ends_at_ts_) {
        st.enabled_ = 0; st.ends_at_ts_ = 0;
    }
    return st;
}
void reply(IpcSocket* c, uint32_t tid, shared::DoNotDisturbOpcode op,
           const shared::DoNotDisturbState& st) {
    shared::XBusMessage m{};
    m.transaction_id_ = tid;
    std::strncpy(m.service_name_, "com.freebsdx.DoNotDisturb", sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(op);
    std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
}  // namespace

bool DoNotDisturbService::isActive() noexcept { return load().enabled_ != 0; }

void DoNotDisturbService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::DoNotDisturbOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState:
        reply(client.get(), msg.transaction_id_, O::GetState, load());
        break;
    case O::SetState: {
        shared::DoNotDisturbRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        shared::DoNotDisturbState st{};
        st.enabled_    = req.enabled_ ? 1u : 0u;
        st.duration_   = req.duration_;
        st.ends_at_ts_ = st.enabled_ ? dnd::endsAt(req.duration_, time(nullptr)) : 0u;
        uint32_t v[3] = {st.enabled_, st.duration_, st.ends_at_ts_};
        toggle::saveInts("dnd", v, 3);
        reply(client.get(), msg.transaction_id_, O::SetState, st);

        shared::XBusMessage b{};
        std::strncpy(b.service_name_, "com.freebsdx.DoNotDisturb", sizeof(b.service_name_) - 1);
        b.opcode_ = static_cast<uint32_t>(O::StateChanged);
        std::memcpy(b.payload_, &st, sizeof(st));
        BroadcastXBusMessage(b);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
