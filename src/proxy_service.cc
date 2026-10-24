#include "xserver/proxy_service.h"
#include "xserver/net_config.h"
#include "shared/logger.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
std::string statePath() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.freebsdx-proxy";
}

shared::ProxyConfig load() {
    std::ifstream f(statePath());
    if (!f.is_open()) return shared::ProxyConfig{};
    std::stringstream ss; ss << f.rdbuf();
    return netcfg::parseProxy(ss.str());
}

// Mirror the proxy into the compositor env so launched apps inherit it. Manual
// mode sets http(s)_proxy + no_proxy; Off/Auto clear them.
void applyEnv(const shared::ProxyConfig& p) {
    if (p.mode_ == 2 /*Manual*/ && p.http_host_[0]) {
        std::string url = "http://" + std::string(p.http_host_) + ":" +
                          std::to_string(p.http_port_);
        setenv("http_proxy", url.c_str(), 1);
        std::string surl = "http://" + std::string(p.http_host_) + ":" +
                           std::to_string(p.https_port_ ? p.https_port_ : p.http_port_);
        setenv("https_proxy", surl.c_str(), 1);
        if (p.no_proxy_[0]) setenv("no_proxy", p.no_proxy_, 1);
    } else {
        unsetenv("http_proxy"); unsetenv("https_proxy"); unsetenv("no_proxy");
    }
}

void reply(IpcSocket* client, uint32_t tid, shared::ProxyOpcode op,
           const shared::ProxyConfig& cfg) {
    shared::XBusMessage m{};
    m.transaction_id_ = tid;
    std::strncpy(m.service_name_, "com.freebsdx.Proxy", sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(op);
    std::memcpy(m.payload_, &cfg, sizeof(cfg));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
}  // namespace

void ProxyService::handleMessage(const shared::XBusMessage& msg,
                                 std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::ProxyOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetConfig:
        reply(client.get(), msg.transaction_id_, O::GetConfig, load());
        break;
    case O::SetConfig: {
        shared::ProxyConfig cfg{};
        std::memcpy(&cfg, msg.payload_, sizeof(cfg));
        // NUL-terminate untrusted char fields before use.
        cfg.pac_url_[sizeof(cfg.pac_url_) - 1]     = '\0';
        cfg.http_host_[sizeof(cfg.http_host_) - 1] = '\0';
        cfg.no_proxy_[sizeof(cfg.no_proxy_) - 1]   = '\0';

        if (std::ofstream f(statePath()); f.is_open())
            f << netcfg::serializeProxy(cfg);
        applyEnv(cfg);
        LOG_INFO("ProxyService: mode set to {}", cfg.mode_);

        reply(client.get(), msg.transaction_id_, O::SetConfig, cfg);

        shared::XBusMessage bcast{};
        std::strncpy(bcast.service_name_, "com.freebsdx.Proxy",
                     sizeof(bcast.service_name_) - 1);
        bcast.opcode_ = static_cast<uint32_t>(O::ConfigChanged);
        std::memcpy(bcast.payload_, &cfg, sizeof(cfg));
        BroadcastXBusMessage(bcast);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
