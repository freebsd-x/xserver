#include "xserver/vpn_service.h"
#include "xserver/ipc_socket.h"
#include "xserver/shell_util.h"
#include "xserver/async_shell.h"
#include "shared/logger.h"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>
#include <iostream>
#include <sys/stat.h>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

static std::string runCmd(const std::string& cmd) noexcept {
    // Async + cached: `wg show`/ifconfig status queries must never block the
    // single-threaded compositor event loop (all callers here are read-only).
    return g_asyncShell.get(cmd);
}

// Parse `wg show <iface>` output for endpoint and transfer bytes.
static void parseWgShow(const std::string& out, shared::VpnStatus& st) noexcept {
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        // Trim leading whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        std::string l = line.substr(start);

        if (l.rfind("endpoint:", 0) == 0) {
            std::string ep = l.substr(9);
            size_t s = ep.find_first_not_of(" \t");
            if (s != std::string::npos)
                std::strncpy(st.endpoint_, ep.c_str() + s, sizeof(st.endpoint_) - 1);
        }
        else if (l.rfind("transfer:", 0) == 0) {
            // "transfer: 1.23 KiB received, 456 B sent"
            double rx_val = 0.0, tx_val = 0.0;
            char rx_unit[8]{}, tx_unit[8]{};
            // sscanf the two numeric values
            if (sscanf(l.c_str(), "transfer: %lf %7s received, %lf %7s sent",
                       &rx_val, rx_unit, &tx_val, tx_unit) == 4) {
                auto toBytes = [](double v, const char* u) -> uint64_t {
                    std::string us(u);
                    if (us == "KiB") return static_cast<uint64_t>(v * 1024);
                    if (us == "MiB") return static_cast<uint64_t>(v * 1024 * 1024);
                    if (us == "GiB") return static_cast<uint64_t>(v * 1024 * 1024 * 1024);
                    return static_cast<uint64_t>(v);
                };
                st.rx_bytes_ = toBytes(rx_val, rx_unit);
                st.tx_bytes_ = toBytes(tx_val, tx_unit);
            }
        }
    }
}

// ── GetStatus ─────────────────────────────────────────────────────────────

shared::VpnStatus VpnService::getStatus() noexcept {
    shared::VpnStatus st{};

    // Detect which wg interfaces are up
    std::string ifaces_out = runCmd("wg show interfaces 2>/dev/null");
    if (ifaces_out.empty()) return st;

    // Take the first reported interface
    std::string iface;
    std::istringstream ss(ifaces_out);
    ss >> iface;
    if (iface.empty()) return st;

    std::strncpy(st.interface_, iface.c_str(), sizeof(st.interface_) - 1);
    st.connected_ = 1;

    // Parse endpoint + transfer stats
    std::string wgout = runCmd("wg show " + iface + " 2>/dev/null");
    parseWgShow(wgout, st);

    // Tunnel IP from ifconfig
    std::string ifout = runCmd("ifconfig " + iface + " inet 2>/dev/null");
    std::istringstream ifs(ifout);
    std::string tok;
    while (ifs >> tok) {
        if (tok == "inet") {
            std::string ip;
            if (ifs >> ip)
                std::strncpy(st.ip_address_, ip.c_str(), sizeof(st.ip_address_) - 1);
            break;
        }
    }

    return st;
}

// ── Broadcast ────────────────────────────────────────────────────────────

static void broadcastVpnChanged(const shared::VpnStatus& st) noexcept {
    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.VPN",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::VpnOpcode::VpnChanged);
    std::memcpy(bcast.payload_, &st, sizeof(st));
    BroadcastXBusMessage(bcast);
}

// ── Connect ───────────────────────────────────────────────────────────────

void VpnService::connectBackground(const char* iface) noexcept {
    std::string name(iface);

    if (name.empty()) {
        // Find the first available .conf in /etc/wireguard/
        std::string listing = runCmd("ls /etc/wireguard/*.conf 2>/dev/null | head -1");
        // Strip trailing newline/spaces
        while (!listing.empty() &&
               (listing.back() == '\n' || listing.back() == '\r' || listing.back() == ' '))
            listing.pop_back();
        if (listing.empty()) {
            LOG_INFO("VPN: no .conf files found in /etc/wireguard/");
            return;
        }
        // Extract basename without extension: /etc/wireguard/wg0.conf → wg0
        size_t slash = listing.rfind('/');
        size_t dot   = listing.rfind('.');
        if (slash != std::string::npos && dot != std::string::npos && dot > slash)
            name = listing.substr(slash + 1, dot - slash - 1);
        else
            name = listing;
    }

    if (!isValidInterfaceName(name)) {
        LOG_INFO("VPN: rejected invalid interface name: {}", name);
        broadcastVpnChanged(VpnService::getStatus());
        return;
    }
    LOG_INFO("VPN: bringing up {}", name);

    // Ensure kernel module is loaded
    (void)fbx::xserver::spawnStatus("kldload if_wg 2>/dev/null");

    int rc = fbx::xserver::spawnStatus(("wg-quick up " + name + " 2>/dev/null").c_str());
    if (rc != 0)
        LOG_INFO("VPN: wg-quick up {} returned {}", name, rc);

    broadcastVpnChanged(VpnService::getStatus());
}

// ── Disconnect ────────────────────────────────────────────────────────────

void VpnService::disconnectBackground(const char* iface) noexcept {
    std::string name(iface);

    if (name.empty()) {
        // Bring down every active wg interface
        std::string ifaces_out = runCmd("wg show interfaces 2>/dev/null");
        std::istringstream ss(ifaces_out);
        std::string tok;
        while (ss >> tok) {
            if (!isValidInterfaceName(tok)) continue; // skip unexpected tokens
            LOG_INFO("VPN: bringing down {}", tok);
            (void)fbx::xserver::spawnStatus(("wg-quick down " + tok + " 2>/dev/null").c_str());
        }
    } else {
        if (!isValidInterfaceName(name)) {
            LOG_INFO("VPN: rejected invalid interface name: {}", name);
            broadcastVpnChanged(VpnService::getStatus());
            return;
        }
        LOG_INFO("VPN: bringing down {}", name);
        (void)fbx::xserver::spawnStatus(("wg-quick down " + name + " 2>/dev/null").c_str());
    }

    broadcastVpnChanged(VpnService::getStatus());
}

// ── ImportConfig ──────────────────────────────────────────────────────────

bool VpnService::importConfig(const char* src_path,
                              char* out_iface, size_t iface_sz) noexcept {
    if (!src_path || src_path[0] == '\0') return false;

    std::string src(src_path);

    // Must end in .conf
    if (src.size() < 6 || src.substr(src.size() - 5) != ".conf") {
        std::cerr << "[VpnService] ImportConfig: not a .conf file: " << src << "\n";
        return false;
    }

    // Extract filename and derive interface name
    size_t slash = src.rfind('/');
    std::string fname = (slash != std::string::npos) ? src.substr(slash + 1) : src;
    std::string iface_name = fname.substr(0, fname.size() - 5); // strip ".conf"

    std::string dst = "/etc/wireguard/" + fname;

    FILE* fsrc = fopen(src.c_str(), "r");
    if (!fsrc) {
        std::cerr << "[VpnService] ImportConfig: cannot open " << src << "\n";
        return false;
    }

    // Ensure /etc/wireguard/ exists
    (void)fbx::xserver::spawnStatus("mkdir -p /etc/wireguard 2>/dev/null");

    FILE* fdst = fopen(dst.c_str(), "w");
    if (!fdst) {
        fclose(fsrc);
        std::cerr << "[VpnService] ImportConfig: cannot write " << dst << "\n";
        return false;
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0)
        fwrite(buf, 1, n, fdst);
    fclose(fsrc);
    fclose(fdst);
    chmod(dst.c_str(), 0600); // WireGuard requires strict permissions

    std::strncpy(out_iface, iface_name.c_str(), iface_sz - 1);
    out_iface[iface_sz - 1] = '\0';

    LOG_INFO("VPN: imported {} → {}", src, dst);
    return true;
}

// ── ProcessMessage ────────────────────────────────────────────────────────

bool VpnService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.VPN") != 0) return false;

    using namespace shared;
    XBusMessage response = msg;

    switch (static_cast<VpnOpcode>(msg.opcode_)) {

    case VpnOpcode::GetStatus: {
        VpnStatus st = getStatus();
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case VpnOpcode::Connect: {
        VpnConnectRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.interface_[sizeof(req.interface_) - 1] = '\0';
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        char iface[16];
        std::strncpy(iface, req.interface_, sizeof(iface));
        std::thread([iface]() { try { connectBackground(iface); } catch (...) {} }).detach();
        break;
    }

    case VpnOpcode::Disconnect: {
        VpnConnectRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.interface_[sizeof(req.interface_) - 1] = '\0';
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        char iface[16];
        std::strncpy(iface, req.interface_, sizeof(iface));
        std::thread([iface]() { try { disconnectBackground(iface); } catch (...) {} }).detach();
        break;
    }

    case VpnOpcode::ImportConfig: {
        VpnImportRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.path_[sizeof(req.path_) - 1] = '\0';

        char iface_name[16]{};
        bool ok = importConfig(req.path_, iface_name, sizeof(iface_name));

        // Response: VpnStatus with connected_=0 on success, 0xFFFFFFFF on error.
        // interface_ carries the derived interface name so the caller knows which to use.
        VpnStatus resp_st{};
        resp_st.connected_ = ok ? 0u : 0xFFFFFFFFu;
        if (ok)
            std::strncpy(resp_st.interface_, iface_name, sizeof(resp_st.interface_) - 1);
        std::memcpy(response.payload_, &resp_st, sizeof(resp_st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    default:
        return false;
    }

    return true;
}

} // namespace fbx::xserver
