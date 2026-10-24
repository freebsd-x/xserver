#include "xserver/dns_service.h"
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
    return std::string(home ? home : "/tmp") + "/.freebsdx-dns";
}
std::string readFile(const char* path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
// Load persisted "mode custom_ip".
void loadState(uint32_t& mode, std::string& custom_ip) {
    mode = netcfg::kDnsAuto; custom_ip.clear();
    std::ifstream f(statePath());
    if (f.is_open()) { f >> mode; std::getline(f, custom_ip);
        // trim leading space from getline after >>
        size_t b = custom_ip.find_first_not_of(" \t");
        custom_ip = (b == std::string::npos) ? "" : custom_ip.substr(b);
    }
}

shared::DnsConfig currentConfig() {
    uint32_t mode; std::string custom_ip;
    loadState(mode, custom_ip);
    std::string ns = netcfg::firstNameserver(readFile("/etc/resolv.conf"));
    return netcfg::buildDnsConfig(mode, custom_ip, ns);
}

void reply(IpcSocket* client, uint32_t tid, shared::DnsOpcode op,
           const shared::DnsConfig& cfg) {
    shared::XBusMessage m{};
    m.transaction_id_ = tid;
    std::strncpy(m.service_name_, "com.freebsdx.DNS", sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(op);
    std::memcpy(m.payload_, &cfg, sizeof(cfg));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
}  // namespace

void DnsService::handleMessage(const shared::XBusMessage& msg,
                               std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::DnsOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetConfig:
        reply(client.get(), msg.transaction_id_, O::GetConfig, currentConfig());
        break;
    case O::SetMode: {
        shared::DnsSetRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.custom_ip_[sizeof(req.custom_ip_) - 1] = '\0';
        uint32_t mode = (req.mode_ <= netcfg::kDnsCustom) ? req.mode_ : netcfg::kDnsAuto;

        // Persist the chosen mode for next boot.
        if (std::ofstream f(statePath()); f.is_open())
            f << mode << ' ' << req.custom_ip_ << '\n';

        // Best-effort resolv.conf write (needs root; Auto leaves DHCP's file).
        std::string body = netcfg::resolvConfFor(mode, req.custom_ip_);
        if (!body.empty()) {
            std::ofstream rf("/etc/resolv.conf");
            if (rf.is_open()) rf << body;
            else LOG_WARN("DnsService: cannot write /etc/resolv.conf (needs root); "
                          "mode persisted only");
        }
        LOG_INFO("DnsService: mode set to {}", mode);

        shared::DnsConfig cfg = currentConfig();
        reply(client.get(), msg.transaction_id_, O::SetMode, cfg);

        shared::XBusMessage bcast{};
        std::strncpy(bcast.service_name_, "com.freebsdx.DNS",
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
