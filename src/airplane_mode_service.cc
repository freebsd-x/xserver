#include "xserver/airplane_mode_service.h"
#include "xserver/toggle_store.h"
#include "xserver/async_shell.h"
#include "xserver/config_service.h"
#include "xserver/priv_broker.h"
#include "xserver/priv_ops.h"
#include "xserver/sysadmin_parse.h"
#include "shared/logger.h"

#include <cstring>
#include <sstream>
#include <string>

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
        if (req.enabled_ && !st.enabled_) {
            // Take down the Wi-Fi interfaces that are up, and remember them.
            auto up = sysadmin::parseWlanIfaces(spawnCapture("ifconfig -l -u 2>/dev/null"));
            std::string downed;
            bool ok = true;
            for (const auto& ifc : up) {
                int r = runPrivilegedOp(privops::kWlanState, ifc + ":0");
                if (r != 0) {
                    ok = false;
                    LOG_WARN("AirplaneMode: can't take {} down ({})", ifc,
                             r == kBrokerUnreachable ? "broker unavailable — needs root" : "broker refused");
                    break;
                }
                downed += (downed.empty() ? "" : ",") + ifc;
            }
            if (ok) {
                st.enabled_ = 1;
                st.wifi_was_on_ = up.empty() ? 0u : 1u;
                st.bt_was_on_ = 0;   // no Bluetooth control on this system
                ConfigService::set("airplane.wlan_down", downed);
                LOG_INFO("AirplaneMode: on (Wi-Fi off: {})", downed.empty() ? "none was up" : downed);
            } else {
                // Restore what we already took down; stay off.
                std::istringstream in(downed);
                for (std::string ifc; std::getline(in, ifc, ',');) runPrivilegedOp(privops::kWlanState, ifc + ":1");
            }
        } else if (!req.enabled_ && st.enabled_) {
            // Bring back exactly the interfaces airplane mode took down.
            std::istringstream in(ConfigService::get("airplane.wlan_down"));
            for (std::string ifc; std::getline(in, ifc, ',');)
                if (runPrivilegedOp(privops::kWlanState, ifc + ":1") != 0)
                    LOG_WARN("AirplaneMode: can't bring {} back up", ifc);
            ConfigService::set("airplane.wlan_down", "");
            st.enabled_ = 0;
            LOG_INFO("AirplaneMode: off");
        }
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
