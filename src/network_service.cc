#include "xserver/network_service.h"
#include "xserver/config_service.h"
#include "xserver/async_shell.h"
#include "xserver/ipc_socket.h"
#include "xserver/shell_util.h"
#include "shared/logger.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <cstdio>
#include <sstream>
#include <regex>
#include <thread>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

// ── Cached scan results (populated by scanBackground()) ──────────────────────
std::vector<shared::WifiNetwork> NetworkService::scan_cache_;
std::mutex NetworkService::scan_mutex_;

// ── Interface helpers ─────────────────────────────────────────────────────────

static std::string findActiveInterface() noexcept {
    static const char* candidates[] = {
        "wlan0", "wlan1", "em0", "igb0", "bge0", "re0", nullptr
    };
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return {};
    std::string result;
    for (int i = 0; candidates[i]; ++i) {
        struct ifreq ifr{};
        std::strncpy(ifr.ifr_name, candidates[i], IFNAMSIZ - 1);
        if (::ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
            result = candidates[i];
            break;
        }
    }
    ::close(fd);
    return result;
}

static std::string findWlanInterface() noexcept {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return {};
    for (int i = 0; i < 4; ++i) {
        std::string name = "wlan" + std::to_string(i);
        struct ifreq ifr{};
        std::strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);
        if (::ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
            ::close(fd);
            return name;
        }
    }
    ::close(fd);
    return {};
}

static std::string runCmd(const std::string& cmd) noexcept {
    // Never block the compositor's event loop: wpa_cli/ifconfig can hang when
    // wpa_supplicant isn't responding. Return the cached output and refresh it
    // on a background thread (see AsyncShellCache).
    return g_asyncShell.get(cmd);
}

// ── GetStatus ─────────────────────────────────────────────────────────────────

shared::NetworkStatus NetworkService::getStatus() noexcept {
    shared::NetworkStatus status{};
    std::strcpy(status.ip_address_, "0.0.0.0");

    // Try wpa_cli first for SSID + signal
    std::string wlan = findWlanInterface();
    if (!wlan.empty()) {
        std::string wpa_out = runCmd("wpa_cli -i " + wlan + " status 2>/dev/null");
        std::istringstream ss(wpa_out);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.rfind("ssid=", 0) == 0)
                std::strncpy(status.current_ssid_, line.c_str() + 5,
                             sizeof(status.current_ssid_) - 1);
            if (line.rfind("wpa_state=COMPLETED", 0) == 0)
                status.is_connected_ = 1;
        }
        // Signal strength via ifconfig
        std::string if_out = runCmd("ifconfig " + wlan + " 2>/dev/null");
        std::smatch m;
        std::regex rssi_re(R"((-\d+)dBm)");
        if (std::regex_search(if_out, m, rssi_re))
            status.signal_strength_ = std::stoi(m[1].str());
    }

    // IP address from ioctl
    std::string iface = findActiveInterface();
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0 && !iface.empty()) {
        struct ifreq ifr{};
        std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
        if (::ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
            auto* addr = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr);
            std::strncpy(status.ip_address_, inet_ntoa(addr->sin_addr),
                         sizeof(status.ip_address_) - 1);
            status.is_connected_ = 1;
        }
        ::close(fd);
    }

    if (status.is_connected_ && status.current_ssid_[0] == '\0')
        std::strncpy(status.current_ssid_,
                     ConfigService::get("net.last_ssid", "").c_str(),
                     sizeof(status.current_ssid_) - 1);

    // Cumulative RX/TX bytes from netstat -ibn (link-layer counters).
    // FreeBSD netstat -ibn format (link line):
    //   Name Mtu Network Address Ipkts Ierrs Idrop Ibytes Opkts Oerrs Obytes Coll
    if (!iface.empty()) {
        std::string nb = runCmd("netstat -ibn -I " + iface + " 2>/dev/null");
        auto pos = nb.find("<Link#");
        if (pos != std::string::npos) {
            size_t ls = nb.rfind('\n', pos);
            ls = (ls == std::string::npos) ? 0 : ls + 1;
            std::istringstream row(nb.substr(ls, nb.find('\n', pos) - ls));
            std::string tok;
            for (int f = 0; row >> tok; ++f) {
                if (f == 7) { try { status.rx_bytes_ = std::stoull(tok); } catch (...) {} }
                if (f == 10) { try { status.tx_bytes_ = std::stoull(tok); } catch (...) {} break; }
            }
        }
    }

    return status;
}

// ── Scan (background + cache) ──────────────────────────────────────────────────

void NetworkService::scanBackground() noexcept {
    std::vector<shared::WifiNetwork> networks;
    std::string wlan = findWlanInterface();
    if (wlan.empty()) { std::lock_guard<std::mutex> lk(scan_mutex_); scan_cache_ = networks; return; }

    LOG_INFO("Wi-Fi scan on {}", wlan);

    // Trigger a fresh scan; wpa_cli scan returns immediately
    (void)fbx::xserver::spawnStatus(("wpa_cli -i " + wlan + " scan >/dev/null 2>&1").c_str());

    // Give the driver ~2s to collect results
    ::sleep(2);

    // Read results: "wpa_cli scan_results" format:
    //   bssid / frequency / signal level / flags / ssid
    std::string out = runCmd("wpa_cli -i " + wlan + " scan_results 2>/dev/null");
    if (out.find("bssid") == std::string::npos) {
        // Fallback: ifconfig wlan scan
        out = runCmd("ifconfig " + wlan + " scan 2>/dev/null");
        std::istringstream ss(out);
        std::string line;
        bool header = true;
        while (std::getline(ss, line)) {
            if (header) { header = false; continue; }
            std::istringstream ls(line);
            std::string ssid, bssid, chan, rate, s_rssi, caps;
            if (!(ls >> ssid >> bssid >> chan >> rate >> s_rssi >> caps)) continue;
            shared::WifiNetwork net{};
            std::strncpy(net.ssid_, ssid.c_str(), sizeof(net.ssid_) - 1);
            std::strncpy(net.bssid_, bssid.c_str(), sizeof(net.bssid_) - 1);
            try { net.signal_strength_ = std::stoi(s_rssi); } catch (...) {}
            bool wpa = caps.find('P') != std::string::npos;
            net.is_secure_     = wpa ? 1u : 0u;
            net.security_type_ = wpa ? 1u : 0u;
            networks.push_back(net);
        }
    } else {
        // wpa_cli scan_results format
        std::istringstream ss(out);
        std::string line;
        bool header = true;
        while (std::getline(ss, line)) {
            if (header) { header = false; continue; } // skip "bssid / ..." header
            if (line.empty()) continue;
            std::istringstream ls(line);
            std::string bssid, freq, signal, flags, ssid;
            if (!(ls >> bssid >> freq >> signal >> flags >> ssid)) continue;
            shared::WifiNetwork net{};
            std::strncpy(net.ssid_,  ssid.c_str(),  sizeof(net.ssid_)  - 1);
            std::strncpy(net.bssid_, bssid.c_str(), sizeof(net.bssid_) - 1);
            try { net.signal_strength_ = std::stoi(signal); } catch (...) {}
            bool wpa2 = flags.find("WPA2") != std::string::npos;
            bool wpa3 = flags.find("SAE")  != std::string::npos;
            net.is_secure_     = (wpa2 || wpa3) ? 1u : 0u;
            net.security_type_ = wpa3 ? 2u : (wpa2 ? 1u : 0u);
            networks.push_back(net);
        }
    }

    size_t n;
    { std::lock_guard<std::mutex> lk(scan_mutex_); scan_cache_ = std::move(networks); n = scan_cache_.size(); }
    LOG_INFO("Scan complete: {} networks", n);

    // Broadcast NetworkChanged so Settings refreshes its list
    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.Network",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::NetworkOpcode::NetworkChanged);
    BroadcastXBusMessage(bcast);
}

// ── Connect ───────────────────────────────────────────────────────────────────

void NetworkService::connect(const shared::WifiConnectRequest& req) noexcept {
    std::string wlan = findWlanInterface();
    if (wlan.empty()) { std::cerr << "[NetworkService] No wlan interface\n"; return; }

    std::string ssid(req.ssid_, strnlen(req.ssid_, sizeof(req.ssid_)));
    std::string pass(req.passphrase_, strnlen(req.passphrase_, sizeof(req.passphrase_)));

    std::cout << "[NetworkService] Connecting to \"" << ssid << "\"\n";

    // Quote SSID and passphrase so any character is safe to pass through the shell.
    std::string q_ssid = shellQuote(ssid);
    std::string q_pass = shellQuote(pass);

    if (pass.empty()) {
        // Open network
        (void)fbx::xserver::spawnStatus(("wpa_cli -i " + wlan
            + " add_network >/dev/null 2>&1").c_str());
        (void)fbx::xserver::spawnStatus(("wpa_cli -i " + wlan
            + " set_network 0 ssid " + q_ssid + " >/dev/null 2>&1").c_str());
        (void)fbx::xserver::spawnStatus(("wpa_cli -i " + wlan
            + " set_network 0 key_mgmt NONE >/dev/null 2>&1").c_str());
    } else {
        // WPA2/3
        (void)fbx::xserver::spawnStatus(("wpa_cli -i " + wlan
            + " add_network >/dev/null 2>&1").c_str());
        (void)fbx::xserver::spawnStatus(("wpa_cli -i " + wlan
            + " set_network 0 ssid " + q_ssid + " >/dev/null 2>&1").c_str());
        (void)fbx::xserver::spawnStatus(("wpa_cli -i " + wlan
            + " set_network 0 psk " + q_pass + " >/dev/null 2>&1").c_str());
    }
    (void)fbx::xserver::spawnStatus(("wpa_cli -i " + wlan
        + " enable_network 0 >/dev/null 2>&1").c_str());
    (void)fbx::xserver::spawnStatus(("wpa_cli -i " + wlan
        + " select_network 0 >/dev/null 2>&1").c_str());

    // Give wpa_supplicant a moment then acquire IP
    ::sleep(3);
    (void)fbx::xserver::spawnStatus(("dhclient " + wlan + " >/dev/null 2>&1").c_str());

    ConfigService::set("net.last_ssid", ssid);

    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.Network",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::NetworkOpcode::NetworkChanged);
    shared::NetworkStatus st = getStatus();
    std::memcpy(bcast.payload_, &st, sizeof(st));
    BroadcastXBusMessage(bcast);
}

// ── Disconnect ────────────────────────────────────────────────────────────────

void NetworkService::disconnect() noexcept {
    std::string wlan = findWlanInterface();
    if (wlan.empty()) return;
    (void)fbx::xserver::spawnStatus(("wpa_cli -i " + wlan + " disconnect >/dev/null 2>&1").c_str());
    std::cout << "[NetworkService] Disconnected\n";

    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.Network",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::NetworkOpcode::NetworkChanged);
    BroadcastXBusMessage(bcast);
}

// ── ProcessMessage ────────────────────────────────────────────────────────────

bool NetworkService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                    const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.Network") != 0) return false;

    using namespace shared;
    XBusMessage response = msg;

    switch (static_cast<NetworkOpcode>(msg.opcode_)) {

    case NetworkOpcode::GetStatus: {
        NetworkStatus status = getStatus();
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &status, sizeof(status));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case NetworkOpcode::Scan: {
        // Start background scan and immediately return current cache page 0
        WifiScanRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t page = req.page_;

        // Kick off a fresh background scan on page 0 request
        if (page == 0)
            std::thread([]() { try { scanBackground(); } catch (...) {} }).detach();

        WifiScanPage pg{};
        pg.page_  = page;
        {
            std::lock_guard<std::mutex> lk(scan_mutex_);
            pg.total_ = static_cast<uint32_t>(scan_cache_.size());
            if (page < scan_cache_.size()) {
                pg.valid_    = 1;
                pg.network_  = scan_cache_[page];
                pg.has_more_ = (page + 1 < scan_cache_.size()) ? 1u : 0u;
            }
        }
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &pg, sizeof(pg));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case NetworkOpcode::Connect: {
        WifiConnectRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        std::thread([req]() { try { connect(req); } catch (...) {} }).detach();
        break;
    }

    case NetworkOpcode::Disconnect:
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        std::thread([]() { try { disconnect(); } catch (...) {} }).detach();
        break;

    default:
        return false;
    }

    return true;
}

} // namespace fbx::xserver
