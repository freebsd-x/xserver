#include "xserver/dns_config_service.h"
#include "xserver/dns_resolver.h"
#include "xserver/net_config.h"
#include "xserver/config_service.h"
#include "xserver/async_shell.h"
#include "xserver/priv_broker.h"
#include "xserver/priv_ops.h"
#include "shared/logger.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
constexpr const char* kSvc = "com.freebsdx.DnsConfig";

std::string readFile(const char* path) {
    std::ifstream f(path);
    if (!f) return "";
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

shared::DnsResolverConfig build() {
    shared::DnsResolverConfig cfg{};
    // Default the primary to the live resolv.conf nameserver when unset.
    std::string def_primary = netcfg::firstNameserver(readFile("/etc/resolv.conf"));
    std::string primary   = ConfigService::get("dnsconfig.primary", def_primary);
    std::string secondary = ConfigService::get("dnsconfig.secondary", "");
    std::string doh_url   = ConfigService::get("dnsconfig.doh_url", "");
    uint32_t doh = 0;
    try { doh = std::stoul(ConfigService::get("dnsconfig.doh_enabled", "0")) ? 1u : 0u; } catch (...) {}

    std::strncpy(cfg.primary_,   primary.c_str(),   sizeof(cfg.primary_) - 1);
    std::strncpy(cfg.secondary_, secondary.c_str(), sizeof(cfg.secondary_) - 1);
    std::strncpy(cfg.doh_url_,   doh_url.c_str(),   sizeof(cfg.doh_url_) - 1);
    cfg.doh_enabled_ = doh;
    return cfg;
}

void sendCfg(std::shared_ptr<IpcSocket>& c, const shared::XBusMessage& req, uint32_t op,
             const shared::DnsResolverConfig& cfg) {
    if (!c) return;
    shared::XBusMessage m{};
    m.transaction_id_ = req.transaction_id_;
    std::strncpy(m.service_name_, kSvc, sizeof(m.service_name_) - 1);
    m.opcode_ = op;
    std::memcpy(m.payload_, &cfg, sizeof(cfg));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

void ack(std::shared_ptr<IpcSocket>& c, const shared::XBusMessage& req, uint32_t op) {
    if (!c) return;
    shared::XBusMessage m{};
    m.transaction_id_ = req.transaction_id_;
    std::strncpy(m.service_name_, kSvc, sizeof(m.service_name_) - 1);
    m.opcode_ = op;
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

void broadcastChanged() {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kSvc, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::DnsConfigOpcode::ConfigChanged);
    auto cfg = build();
    std::memcpy(b.payload_, &cfg, sizeof(cfg));
    BroadcastXBusMessage(b);
}
}  // namespace

void DnsConfigService::handleMessage(const shared::XBusMessage& msg,
                                     std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::DnsConfigOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetConfig:
        { auto cfg = build(); sendCfg(client, msg, static_cast<uint32_t>(O::GetConfig), cfg); }
        break;
    case O::SetConfig: {
        shared::DnsResolverConfig req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.primary_[sizeof(req.primary_) - 1]     = '\0';
        req.secondary_[sizeof(req.secondary_) - 1] = '\0';
        req.doh_url_[sizeof(req.doh_url_) - 1]      = '\0';

        if (!dnsresolver::validField(req.primary_) || !dnsresolver::validField(req.secondary_)) {
            LOG_WARN("DnsConfigService: rejected invalid resolver '{}' / '{}'",
                     req.primary_, req.secondary_);
            ack(client, msg, static_cast<uint32_t>(O::SetConfig));   // ack, but don't persist
            break;
        }
        ConfigService::set("dnsconfig.primary",     req.primary_);
        ConfigService::set("dnsconfig.secondary",   req.secondary_);
        ConfigService::set("dnsconfig.doh_url",     req.doh_url_);
        ConfigService::set("dnsconfig.doh_enabled", req.doh_enabled_ ? "1" : "0");

        // Apply resolv.conf via the root privilege broker (xsessiond); fall back
        // to a direct best-effort write when the broker isn't running.
        std::string body = dnsresolver::resolvConfBody(req.primary_, req.secondary_);
        if (!body.empty()) {
            int r = runPrivilegedOp(privops::kResolvConfWrite, body);
            if (r == 0) {
                LOG_INFO("DnsConfigService: resolv.conf applied via broker");
            } else {
                if (r != kBrokerUnreachable)
                    LOG_WARN("DnsConfigService: broker refused resolv.conf write ({})", r);
                std::ofstream f("/etc/resolv.conf");   // fallback (works only as root)
                if (f) f << body;
                else LOG_INFO("DnsConfigService: resolv.conf not writable (persisted only)");
            }
        }
        ack(client, msg, static_cast<uint32_t>(O::SetConfig));
        broadcastChanged();
        break;
    }
    case O::Flush: {
        // Reload the local resolver cache (needs root → best-effort, logged).
        int rc = spawnStatus("service local_unbound reload >/dev/null 2>&1");
        if (rc != 0) LOG_INFO("DnsConfigService: DNS cache flush needs root (service local_unbound reload)");
        ack(client, msg, static_cast<uint32_t>(O::Flush));
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
