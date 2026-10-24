#include "xserver/gamemode_service.h"
#include "xserver/toggle_store.h"
#include <cstring>
#include <ctime>

namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
// Persist enabled + the timestamp it was enabled (for elapsed_secs).
shared::GameModeState load() {
    shared::GameModeState st{};
    uint32_t v[2] = {0, 0};   // {enabled, since_ts}
    toggle::loadInts("gamemode", v, 2);
    st.enabled_ = v[0];
    uint32_t now = static_cast<uint32_t>(time(nullptr));
    st.elapsed_secs_ = (v[0] && v[1] && now >= v[1]) ? (now - v[1]) : 0u;
    return st;
}
}  // namespace

void GameModeService::handleMessage(const shared::XBusMessage& msg,
                                    std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::GameModeOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState: {
        shared::GameModeState st = load();
        shared::XBusMessage m{}; m.transaction_id_ = msg.transaction_id_;
        std::strncpy(m.service_name_, "com.freebsdx.GameMode", sizeof(m.service_name_) - 1);
        m.opcode_ = static_cast<uint32_t>(O::GetState);
        std::memcpy(m.payload_, &st, sizeof(st));
        client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
        break;
    }
    case O::SetEnabled: {   // protocol: no response
        shared::GameModeEnableRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t enabled = req.enabled_ ? 1u : 0u;
        uint32_t v[2] = {enabled, enabled ? static_cast<uint32_t>(time(nullptr)) : 0u};
        toggle::saveInts("gamemode", v, 2);

        shared::GameModeState st = load();
        shared::XBusMessage b{};
        std::strncpy(b.service_name_, "com.freebsdx.GameMode", sizeof(b.service_name_) - 1);
        b.opcode_ = static_cast<uint32_t>(O::StateChanged);
        std::memcpy(b.payload_, &st, sizeof(st));
        BroadcastXBusMessage(b);
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
