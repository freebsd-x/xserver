#include "xserver/bluetooth_service.h"
#include "xserver/ipc_socket.h"
#include "xserver/shell_util.h"
#include "xserver/async_shell.h"
#include <cstring>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <thread>
#include <regex>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

std::atomic<bool> BluetoothService::scanning_{false};
std::vector<shared::BluetoothDevice> BluetoothService::known_devices_;
std::mutex BluetoothService::devices_mutex_;

// ── CLI helpers ───────────────────────────────────────────────────────────────

std::string BluetoothService::runCmd(const std::string& cmd) noexcept {
    // Async + cached: hccontrol can hang on a wedged adapter, and this runs on
    // the compositor's single event-loop thread — never block it.
    return g_asyncShell.get(cmd);
}

bool BluetoothService::adapterPresent() noexcept {
    // Cached exit-code check (emit a token on success so we can read it back).
    return g_asyncShell.get(
        "hccontrol -n hci0 read_bd_addr >/dev/null 2>&1 && echo ok").find("ok")
        != std::string::npos;
}

bool BluetoothService::adapterEnabled() noexcept {
    return g_asyncShell.get(
        "hccontrol -n hci0 read_local_features >/dev/null 2>&1 && echo ok").find("ok")
        != std::string::npos;
}

void BluetoothService::setEnabled(bool on) noexcept {
    if (on) {
        runDetached("service bluetooth start hci0 2>/dev/null");
        std::cout << "[BluetoothService] Adapter enabled\n";
    } else {
        runDetached("service bluetooth stop hci0 2>/dev/null");
        std::cout << "[BluetoothService] Adapter disabled\n";
    }
}

shared::BluetoothState BluetoothService::getState() noexcept {
    shared::BluetoothState st{};
    st.present_  = adapterPresent()  ? 1u : 0u;
    st.enabled_  = adapterEnabled()  ? 1u : 0u;
    st.scanning_ = scanning_.load()  ? 1u : 0u;
    return st;
}

// Parse `hccontrol -n hci0 read_stored_link_key` and `hccontrol inquiry` output.
// Each paired entry looks like:
//   BD_ADDR: 00:11:22:33:44:55
//   (name may follow in a subsequent line)
std::vector<shared::BluetoothDevice> BluetoothService::listDevices() noexcept {
    std::vector<shared::BluetoothDevice> devs;
    { std::lock_guard<std::mutex> lk(devices_mutex_); devs = known_devices_; }

    // Augment with paired keys from the host controller store
    std::string keys_out = runCmd("hccontrol -n hci0 read_stored_link_key 2>/dev/null");
    std::istringstream ss(keys_out);
    std::string line;
    std::regex addr_re(R"(BD_ADDR:\s*([0-9a-fA-F:]{17}))");
    while (std::getline(ss, line)) {
        std::smatch m;
        if (!std::regex_search(line, m, addr_re)) continue;
        std::string addr = m[1].str();
        // Check if already in list
        bool found = false;
        for (auto& d : devs)
            if (std::strncmp(d.addr_.str_, addr.c_str(), 17) == 0) {
                d.paired_ = 1;
                found = true;
                break;
            }
        if (!found) {
            shared::BluetoothDevice d{};
            std::strncpy(d.addr_.str_, addr.c_str(), sizeof(d.addr_.str_) - 1);
            std::strncpy(d.name_, addr.c_str(), sizeof(d.name_) - 1); // addr as fallback name
            d.paired_    = 1;
            d.connected_ = 0;
            devs.push_back(d);
        }
    }
    return devs;
}

// ── Background scan ───────────────────────────────────────────────────────────

void BluetoothService::runScan() noexcept {
    scanning_ = true;
    std::cout << "[BluetoothService] Inquiry started\n";

    // hccontrol inquiry runs for ~10s and prints one device per block:
    //   Inquiry result, num_responses=1
    //   BD_ADDR: 00:11:22:33:44:55
    //   Page Scan Rep. Mode: 0x01
    //   Class: ...
    //   Clock offset: 0x...
    std::string out = runCmd("hccontrol -n hci0 inquiry 2>/dev/null");

    std::istringstream ss(out);
    std::string line;
    std::regex addr_re(R"(BD_ADDR:\s*([0-9a-fA-F:]{17}))");
    std::regex name_re(R"(Name:\s*(.+))");

    shared::BluetoothDevice cur{};
    bool have_dev = false;

    auto flush = [&]() {
        if (!have_dev) return;
        // Resolve name via hccontrol remote_name_request (best-effort, quick timeout)
        if (std::strncmp(cur.name_, cur.addr_.str_, 17) == 0) {
            std::string nc = runCmd(std::string("hccontrol -n hci0 remote_name_request ")
                                    + cur.addr_.str_ + " 2>/dev/null");
            std::smatch nm;
            if (std::regex_search(nc, nm, name_re))
                std::strncpy(cur.name_, nm[1].str().c_str(), sizeof(cur.name_) - 1);
        }
        // Mark paired if we have a stored link key for this address
        {
            std::lock_guard<std::mutex> lk(devices_mutex_);
            for (const auto& k : known_devices_)
                if (std::strncmp(k.addr_.str_, cur.addr_.str_, 17) == 0)
                    cur.paired_ = k.paired_;
        }

        // Broadcast DeviceFound
        shared::XBusMessage bcast{};
        bcast.magic_ = shared::XBUS_MAGIC;
        std::strncpy(bcast.service_name_, "com.freebsdx.Bluetooth",
                     sizeof(bcast.service_name_) - 1);
        bcast.opcode_ = static_cast<uint32_t>(shared::BluetoothOpcode::DeviceFound);
        std::memcpy(bcast.payload_, &cur, sizeof(cur));
        BroadcastXBusMessage(bcast);

        // Keep in known_devices_ for ListDevices
        {
            std::lock_guard<std::mutex> lk(devices_mutex_);
            bool found = false;
            for (auto& d : known_devices_)
                if (std::strncmp(d.addr_.str_, cur.addr_.str_, 17) == 0) {
                    d = cur; found = true; break;
                }
            if (!found) known_devices_.push_back(cur);
        }

        cur = {};
        have_dev = false;
    };

    while (std::getline(ss, line)) {
        std::smatch m;
        if (std::regex_search(line, m, addr_re)) {
            flush();
            std::strncpy(cur.addr_.str_, m[1].str().c_str(), sizeof(cur.addr_.str_) - 1);
            std::strncpy(cur.name_,      m[1].str().c_str(), sizeof(cur.name_) - 1);
            have_dev = true;
        }
    }
    flush();

    scanning_ = false;
    std::cout << "[BluetoothService] Inquiry complete\n";
}

// ── ProcessMessage ────────────────────────────────────────────────────────────

void BluetoothService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                      const shared::XBusMessage& msg) noexcept {
    using namespace shared;
    XBusMessage resp = msg;

    switch (static_cast<BluetoothOpcode>(msg.opcode_)) {

    case BluetoothOpcode::GetState: {
        BluetoothState st = getState();
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &st, sizeof(st));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case BluetoothOpcode::SetEnabled: {
        BluetoothEnableRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        setEnabled(req.enabled_ != 0);
        BluetoothState st = getState();
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &st, sizeof(st));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case BluetoothOpcode::Scan: {
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        if (!scanning_) {
            std::thread([]() { try { runScan(); } catch (...) {} }).detach();
        }
        break;
    }

    case BluetoothOpcode::StopScan:
        scanning_ = false; // runScan() checks this; inquiry will still finish
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;

    case BluetoothOpcode::ListDevices: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));
        auto devs = listDevices();
        BluetoothDevicePage pg{};
        pg.page_     = page;
        pg.total_    = static_cast<uint32_t>(devs.size());
        pg.has_more_ = 0;
        if (page < devs.size()) {
            pg.count_      = 1;
            pg.devices_[0] = devs[page];
            pg.has_more_   = (page + 1 < devs.size()) ? 1u : 0u;
        }
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &pg, sizeof(pg));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case BluetoothOpcode::Connect: {
        BluetoothDeviceRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        if (!isValidMacAddress(req.addr_.str_)) {
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
            break;
        }
        // Connect HID device via bthidcontrol
        std::string cmd = std::string("bthidcontrol -a ") + req.addr_.str_
                          + " -c /etc/bluetooth/bthidd.conf connect 2>/dev/null";
        (void)fbx::xserver::spawnStatus(cmd.c_str());
        std::cout << "[BluetoothService] Connect: " << req.addr_.str_ << "\n";
        for (auto& d : known_devices_)
            if (std::strncmp(d.addr_.str_, req.addr_.str_, 17) == 0)
                d.connected_ = 1;
        // Broadcast DeviceChanged
        XBusMessage bcast = resp;
        bcast.opcode_ = static_cast<uint32_t>(BluetoothOpcode::DeviceChanged);
        BroadcastXBusMessage(bcast);
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case BluetoothOpcode::Disconnect: {
        BluetoothDeviceRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        if (!isValidMacAddress(req.addr_.str_)) {
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
            break;
        }
        std::string cmd = std::string("bthidcontrol -a ") + req.addr_.str_
                          + " -c /etc/bluetooth/bthidd.conf disconnect 2>/dev/null";
        (void)fbx::xserver::spawnStatus(cmd.c_str());
        std::cout << "[BluetoothService] Disconnect: " << req.addr_.str_ << "\n";
        for (auto& d : known_devices_)
            if (std::strncmp(d.addr_.str_, req.addr_.str_, 17) == 0)
                d.connected_ = 0;
        XBusMessage bcast = resp;
        bcast.opcode_ = static_cast<uint32_t>(BluetoothOpcode::DeviceChanged);
        BroadcastXBusMessage(bcast);
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case BluetoothOpcode::Pair: {
        BluetoothDeviceRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        if (!isValidMacAddress(req.addr_.str_)) {
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
            break;
        }
        // Initiate pairing: hccontrol create_connection + hcsecd handles key exchange
        std::string cmd = std::string("hccontrol -n hci0 create_connection ")
                          + req.addr_.str_ + " 2>/dev/null";
        (void)fbx::xserver::spawnStatus(cmd.c_str());
        std::cout << "[BluetoothService] Pair: " << req.addr_.str_ << "\n";
        for (auto& d : known_devices_)
            if (std::strncmp(d.addr_.str_, req.addr_.str_, 17) == 0)
                d.paired_ = 1;
        XBusMessage bcast = resp;
        bcast.opcode_ = static_cast<uint32_t>(BluetoothOpcode::DeviceChanged);
        BroadcastXBusMessage(bcast);
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case BluetoothOpcode::Forget: {
        BluetoothDeviceRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        if (!isValidMacAddress(req.addr_.str_)) {
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
            break;
        }
        // Remove stored link key
        std::string cmd = std::string("hccontrol -n hci0 delete_stored_link_key ")
                          + req.addr_.str_ + " 2>/dev/null";
        (void)fbx::xserver::spawnStatus(cmd.c_str());
        std::cout << "[BluetoothService] Forget: " << req.addr_.str_ << "\n";
        known_devices_.erase(
            std::remove_if(known_devices_.begin(), known_devices_.end(),
                [&req](const BluetoothDevice& d) {
                    return std::strncmp(d.addr_.str_, req.addr_.str_, 17) == 0;
                }),
            known_devices_.end());
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    default:
        std::cerr << "[BluetoothService] Unknown opcode: 0x"
                  << std::hex << msg.opcode_ << "\n";
        break;
    }
}

} // namespace fbx::xserver
