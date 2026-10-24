#include "xserver/location_service.h"
#include "xserver/location.h"
#include "xserver/config_service.h"
#include "shared/logger.h"

#include <cstring>
#include <string>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
bool cfgEnabled() {
    try { return std::stoul(ConfigService::get("location.enabled", "0")) != 0; }
    catch (...) { return false; }
}

shared::LocationState buildState() {
    shared::LocationState st{};
    st.enabled_          = cfgEnabled() ? 1u : 0u;
    st.requesting_count_ = 0;   // no location provider wired
    st.last_app_[0]      = '\0';
    return st;
}

// Reply echoing the request's service name (LocationServices vs Location).
void reply(std::shared_ptr<IpcSocket>& c, const shared::XBusMessage& req,
           uint32_t op, const void* payload, size_t len) {
    if (!c) return;
    shared::XBusMessage m{};
    m.transaction_id_ = req.transaction_id_;
    std::strncpy(m.service_name_, req.service_name_, sizeof(m.service_name_) - 1);
    m.opcode_ = op;
    if (payload && len) std::memcpy(m.payload_, payload, len);
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

void broadcast(const char* service, uint32_t op, const void* payload, size_t len) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, service, sizeof(b.service_name_) - 1);
    b.opcode_ = op;
    if (payload && len) std::memcpy(b.payload_, payload, len);
    BroadcastXBusMessage(b);
}
}  // namespace

void LocationService::handleMessage(const shared::XBusMessage& msg,
                                    std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::LocationOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState:
        { auto st = buildState(); reply(client, msg, static_cast<uint32_t>(O::GetState), &st, sizeof(st)); }
        break;
    case O::SetEnabled: {
        shared::LocationEnableRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        ConfigService::set("location.enabled", req.enabled_ ? "1" : "0");
        auto st = buildState();
        reply(client, msg, static_cast<uint32_t>(O::SetEnabled), &st, sizeof(st));   // ack
        broadcast("com.freebsdx.LocationServices",
                  static_cast<uint32_t>(O::StateChanged), &st, sizeof(st));
        break;
    }
    case O::ListApps: {
        // No app registry yet → empty page (honest; apps populate it via SetAppPolicy).
        shared::LocationAppPage pg{};
        std::memcpy(&pg.page_, msg.payload_, sizeof(pg.page_));
        pg.total_ = 0; pg.has_more_ = 0; pg.valid_ = 0;
        reply(client, msg, static_cast<uint32_t>(O::ListApps), &pg, sizeof(pg));
        break;
    }
    case O::SetAppPolicy: {
        shared::LocationAppPolicyRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.identifier_[sizeof(req.identifier_) - 1] = '\0';
        uint32_t prec = location::clampPrecision(req.precision_);
        if (req.identifier_[0])
            ConfigService::set(std::string("location.app.") + req.identifier_, std::to_string(prec));
        reply(client, msg, static_cast<uint32_t>(O::SetAppPolicy), nullptr, 0);   // ack
        broadcast("com.freebsdx.Location", static_cast<uint32_t>(O::PolicyChanged), nullptr, 0);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
