#include "xserver/savedwifi_service.h"
#include "xserver/wifi_saved.h"
#include "shared/logger.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
std::vector<wifi::SavedNet> readSaved() {
    std::ifstream f("/etc/wpa_supplicant.conf");
    if (!f.is_open()) {
        LOG_WARN("SavedWifiService: /etc/wpa_supplicant.conf unreadable "
                 "(needs root); returning empty list");
        return {};
    }
    std::stringstream ss; ss << f.rdbuf();
    return wifi::parseWpaSupplicant(ss.str());
}

void ackChanged(IpcSocket* client, uint32_t tid, shared::SavedWifiOpcode op) {
    shared::XBusMessage m{};
    m.transaction_id_ = tid;
    std::strncpy(m.service_name_, "com.freebsdx.SavedWifi", sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(op);
    client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
}  // namespace

void SavedWifiService::handleMessage(const shared::XBusMessage& msg,
                                     std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::SavedWifiOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListSaved: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));
        auto nets = readSaved();

        shared::SavedWifiPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(nets.size());
        if (page < nets.size()) {
            const wifi::SavedNet& n = nets[page];
            std::strncpy(pg.ssid_, n.ssid.c_str(), sizeof(pg.ssid_) - 1);
            pg.security_type_  = n.security;
            pg.auto_connect_   = n.auto_connect;
            pg.last_connected_ = 0;  // wpa_supplicant.conf doesn't record this
            pg.valid_          = 1;
        }
        pg.has_more_ = (page + 1 < nets.size()) ? 1u : 0u;

        shared::XBusMessage reply{};
        reply.transaction_id_ = msg.transaction_id_;
        std::strncpy(reply.service_name_, "com.freebsdx.SavedWifi",
                     sizeof(reply.service_name_) - 1);
        reply.opcode_ = static_cast<uint32_t>(O::ListSaved);
        std::memcpy(reply.payload_, &pg, sizeof(pg));
        client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
        break;
    }
    case O::SetAutoConnect: {
        shared::SavedWifiAutoConnectRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.ssid_[sizeof(req.ssid_) - 1] = '\0';
        // Applying this needs write access to wpa_supplicant.conf / wpa_cli
        // (root). Logged; a privileged helper is the eventual path.
        LOG_INFO("SavedWifiService: set auto-connect={} for '{}' (needs root to apply)",
                 req.auto_connect_, req.ssid_);
        ackChanged(client.get(), msg.transaction_id_, O::SetAutoConnect);
        break;
    }
    case O::ForgetNetwork: {
        shared::SavedWifiForgetRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.ssid_[sizeof(req.ssid_) - 1] = '\0';
        LOG_INFO("SavedWifiService: forget '{}' (needs root to apply)", req.ssid_);
        ackChanged(client.get(), msg.transaction_id_, O::ForgetNetwork);
        // Broadcast so panels re-fetch (the list itself is authoritative on read).
        shared::XBusMessage bcast{};
        std::strncpy(bcast.service_name_, "com.freebsdx.SavedWifi",
                     sizeof(bcast.service_name_) - 1);
        bcast.opcode_ = static_cast<uint32_t>(O::SavedListChanged);
        BroadcastXBusMessage(bcast);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
