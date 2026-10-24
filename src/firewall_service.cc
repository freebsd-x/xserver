#include "xserver/firewall_service.h"
#include "xserver/firewall.h"
#include "xserver/config_service.h"
#include "xserver/async_shell.h"
#include "xserver/priv_broker.h"
#include "xserver/priv_ops.h"
#include "shared/logger.h"

#include <cstring>
#include <string>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
constexpr const char* kSvc = "com.freebsdx.Firewall";

shared::FirewallStatus build() {
    shared::FirewallStatus st{};
    // Reads need root; unprivileged → empty output → disabled/0 (honest).
    std::string info  = spawnCapture("pfctl -s info 2>/dev/null");
    std::string rules = spawnCapture("pfctl -s rules 2>/dev/null");
    st.enabled_    = firewall::parseEnabled(info) ? 1u : 0u;
    st.rule_count_ = firewall::countRules(rules);
    uint32_t ba = 0;
    try { ba = std::stoul(ConfigService::get("firewall.block_all", "0")) ? 1u : 0u; } catch (...) {}
    st.block_all_ = ba;
    return st;
}

void sendStatus(std::shared_ptr<IpcSocket>& c, const shared::XBusMessage& req, uint32_t op) {
    if (!c) return;
    auto st = build();
    shared::XBusMessage m{};
    m.transaction_id_ = req.transaction_id_;
    std::strncpy(m.service_name_, kSvc, sizeof(m.service_name_) - 1);
    m.opcode_ = op;
    std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

void broadcastStatus() {
    auto st = build();
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kSvc, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::FirewallOpcode::StatusChanged);
    std::memcpy(b.payload_, &st, sizeof(st));
    BroadcastXBusMessage(b);
}
}  // namespace

void FirewallService::handleMessage(const shared::XBusMessage& msg,
                                    std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::FirewallOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetStatus:
        sendStatus(client, msg, static_cast<uint32_t>(O::GetStatus));
        break;
    case O::SetEnabled: {
        shared::FirewallEnableRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        const char* arg = req.enabled_ ? "1" : "0";
        int r = runPrivilegedOp(privops::kFirewallEnable, arg);
        if (r == 0) {
            LOG_INFO("FirewallService: pf {} via broker", req.enabled_ ? "enabled" : "disabled");
        } else {
            if (r != kBrokerUnreachable)
                LOG_WARN("FirewallService: broker refused pf enable/disable ({})", r);
            int rc = spawnStatus(std::string("pfctl ") + (req.enabled_ ? "-e" : "-d") + " >/dev/null 2>&1");
            if (rc != 0) LOG_INFO("FirewallService: pfctl needs root (not applied)");
        }
        sendStatus(client, msg, static_cast<uint32_t>(O::SetEnabled));   // ack = fresh status
        broadcastStatus();
        break;
    }
    case O::BlockAllInbound:
        // Record the emergency flag; loading the actual block-all-inbound pf
        // ruleset is a follow-up (needs a ruleset-load broker verb).
        ConfigService::set("firewall.block_all", "1");
        LOG_INFO("FirewallService: block-all-inbound flagged (ruleset load is a follow-up)");
        sendStatus(client, msg, static_cast<uint32_t>(O::BlockAllInbound));
        broadcastStatus();
        break;
    default: break;
    }
}

}  // namespace fbx::xserver
