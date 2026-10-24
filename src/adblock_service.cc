#include "xserver/adblock_service.h"
#include "xserver/adblock.h"
#include "xserver/config_service.h"
#include "shared/logger.h"

#include <cstring>
#include <ctime>
#include <string>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
constexpr const char* kSvc = "com.freebsdx.AdBlock";

uint32_t cfgUint(const char* key, uint32_t dflt) {
    try { return static_cast<uint32_t>(std::stoul(ConfigService::get(key, std::to_string(dflt)))); }
    catch (...) { return dflt; }
}

shared::AdBlockStatus build() {
    shared::AdBlockStatus st{};
    st.enabled_      = cfgUint("adblock.enabled", 0) ? 1u : 0u;
    st.domain_count_ = cfgUint("adblock.domain_count", 0);
    std::string date = ConfigService::get("adblock.list_date", "never");
    std::strncpy(st.list_updated_, date.c_str(), sizeof(st.list_updated_) - 1);
    st.updating_ = 0;   // synchronous here (no background fetch)
    return st;
}

void bcast() {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kSvc, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::AdBlockOpcode::StatusChanged);
    auto st = build();
    std::memcpy(b.payload_, &st, sizeof(st));
    BroadcastXBusMessage(b);
}
}  // namespace

void AdBlockService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::AdBlockOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetStatus: {
        auto st = build();
        shared::XBusMessage reply{};
        reply.transaction_id_ = msg.transaction_id_;
        std::strncpy(reply.service_name_, kSvc, sizeof(reply.service_name_) - 1);
        reply.opcode_ = static_cast<uint32_t>(O::GetStatus);
        std::memcpy(reply.payload_, &st, sizeof(st));
        if (client) client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
        break;
    }
    case O::SetEnabled: {
        shared::AdBlockEnableRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        ConfigService::set("adblock.enabled", req.enabled_ ? "1" : "0");
        LOG_INFO("AdBlockService: {} (persisted; DNS sinkhole is a follow-up)",
                 req.enabled_ ? "enabled" : "disabled");
        bcast();   // no direct reply per contract
        break;
    }
    case O::UpdateList: {
        // No blocklist fetch here (needs network/root) — stamp the refresh date
        // and recount from any locally-stored list (0 if none) + confirm.
        ConfigService::set("adblock.list_date", adblock::formatListDate(std::time(nullptr)));
        LOG_INFO("AdBlockService: list refresh stamped (real blocklist fetch is a follow-up)");
        bcast();   // StatusChanged on completion, no direct reply
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
