#include "xserver/extras_services.h"
#include "xserver/extras_parse.h"
#include "xserver/async_shell.h"
#include "xserver/config_service.h"
#include "xserver/shell_util.h"
#include "xserver/sysadmin_parse.h"
#include "xserver/user_files.h"
#include "xserver/xbus_reply.h"
#include "shared/logger.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace fbx::xserver {

namespace {

using namespace svc;

// ── Hotspot ──────────────────────────────────────────────────────────────────

// Why there's no hotspot on this machine (for the log).
std::string hotspotUnavailableReason() {
    auto wlans = sysadmin::parseWlanIfaces(spawnCapture("ifconfig -l 2>/dev/null"));
    if (wlans.empty()) return "there's no Wi-Fi interface";
    for (const auto& w : wlans)
        if (extras::driverSupportsHostap(spawnCapture("ifconfig " + w + " list caps 2>/dev/null")))
            return "access-point setup (hostapd on a hostap-mode wlan) isn't implemented yet";
    return "the Wi-Fi driver doesn't support access-point (HOSTAP) mode";
}

// ── Bluetooth ────────────────────────────────────────────────────────────────

bool haveBluetoothController() {
    std::string tree = spawnCapture("devinfo 2>/dev/null");
    for (std::string_view drv : {"ubt", "iwmbt", "iwbt", "ng_ubt"}) {
        size_t p = 0;
        while ((p = tree.find(drv, p)) != std::string::npos) {
            size_t e = p + drv.size();
            if (e < tree.size() && std::isdigit(static_cast<unsigned char>(tree[e]))) return true;
            p = e;
        }
    }
    return false;
}

struct BtDevice { std::string addr, name; };

// /etc/bluetooth/hosts: "<bdaddr> <name>".
std::vector<BtDevice> knownBtDevices() {
    std::vector<BtDevice> out;
    std::istringstream in(userfiles::readFile("/etc/bluetooth/hosts").value_or(""));
    std::string line;
    while (std::getline(in, line)) {
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream f(line);
        std::string addr, name;
        if (f >> addr >> name && std::count(addr.begin(), addr.end(), ':') == 5) out.push_back({addr, name});
    }
    return out;
}

// ── Syncthing ────────────────────────────────────────────────────────────────

struct Syncthing {
    std::string apikey, address = "127.0.0.1:8384";
    std::vector<extras::SyncFolder> folders;
    bool present = false;
};

Syncthing loadSyncthing() {
    Syncthing st;
    const std::string home = userfiles::homeDir();
    for (const std::string& path : {home + "/.local/state/syncthing/config.xml",
                                    home + "/.config/syncthing/config.xml"}) {
        auto xml = userfiles::readFile(path);
        if (!xml) continue;
        st.present = true;
        st.folders = extras::parseFolders(*xml);
        st.apikey  = extras::parseApiKey(*xml);
        size_t g = xml->find("<gui ");
        if (g != std::string::npos) {
            size_t a = xml->find("<address>", g), e = xml->find("</address>", g);
            if (a != std::string::npos && e != std::string::npos && e > a + 9)
                st.address = xml->substr(a + 9, e - a - 9);
        }
        break;
    }
    // Both go onto a curl command line: keep them to what Syncthing produces.
    auto ok = [](const std::string& s, std::string_view extra) {
        return !s.empty() && s.size() <= 64 &&
               std::all_of(s.begin(), s.end(), [&](char c) {
                   return std::isalnum(static_cast<unsigned char>(c)) || extra.find(c) != std::string_view::npos;
               });
    };
    if (!ok(st.apikey, "-_")) st.apikey.clear();
    if (!ok(st.address, ".:[]")) st.address = "127.0.0.1:8384";
    return st;
}

std::string restGet(const Syncthing& st, const std::string& path) {
    if (st.apikey.empty()) return {};
    return spawnCapture("curl -sf -m 3 -H " + shellQuote("X-API-Key: " + st.apikey) + " " +
                        shellQuote("http://" + st.address + path) + " 2>/dev/null");
}

}  // namespace

// ── HotspotService ───────────────────────────────────────────────────────────

void HotspotService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    constexpr const char* kName = "com.freebsdx.Hotspot";
    // HotspotState is HotspotStatus plus a passphrase hint: one reply fits both.
    shared::HotspotState st{};
    switch (msg.opcode_) {
    case 0x01:   // GetStatus / GetState
        reply(msg, client, kName, msg.opcode_, &st);
        break;
    case 0x02: { // SetEnabled (both shapes: enabled_ first)
        uint32_t want = 0;
        std::memcpy(&want, msg.payload_, sizeof(want));
        if (want) LOG_WARN("Hotspot: can't start a hotspot — {}", hotspotUnavailableReason());
        ack(msg, client, kName);
        // The real state, so the switch snaps back.
        broadcast(kName, static_cast<uint32_t>(shared::HotspotOpcode::StatusChanged), &st);
        broadcast(kName, static_cast<uint32_t>(shared::HotspotToggleOpcode::StateChanged), &st);
        break;
    }
    case 0x03:   // Configure
        LOG_INFO("Hotspot: configuration ignored — {}", hotspotUnavailableReason());
        ack(msg, client, kName);
        break;
    default: break;
    }
}

// ── BtAudioService ───────────────────────────────────────────────────────────

void BtAudioService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::BtAudioOpcode;
    constexpr const char* kName = "com.freebsdx.BtAudio";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListDevices: {
        auto devs = haveBluetoothController() ? knownBtDevices() : std::vector<BtDevice>{};
        uint32_t page = readPage(msg);
        shared::BtAudioPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(devs.size());
        if (page < devs.size()) {
            pg.valid_       = 1;
            pg.has_more_    = page + 1 < devs.size() ? 1u : 0u;
            std::strncpy(pg.address_, devs[page].addr.c_str(), sizeof(pg.address_) - 1);
            std::strncpy(pg.name_, devs[page].name.c_str(), sizeof(pg.name_) - 1);
            pg.battery_pct_ = 255;   // unknown
        }
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::Connect:
    case O::Disconnect:
        LOG_WARN("BtAudio: {}", haveBluetoothController()
                                    ? "no Bluetooth audio stack (virtual_oss/A2DP) is set up"
                                    : "there's no Bluetooth controller");
        ack(msg, client, kName);
        break;
    default: break;
    }
}

// ── FileSyncService ──────────────────────────────────────────────────────────

void FileSyncService::handleMessage(const shared::XBusMessage& msg,
                                    std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::FileSyncOpcode;
    constexpr const char* kName = "com.freebsdx.FileSync";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListJobs: {
        Syncthing st = loadSyncthing();
        auto pg = makePage<shared::FileSyncPage>(readPage(msg), st.folders,
            [&st](shared::FileSyncEntry& e, const extras::SyncFolder& f) {
                std::strncpy(e.name_, f.label.c_str(), sizeof(e.name_) - 1);
                std::strncpy(e.path_, f.path.c_str(), sizeof(e.path_) - 1);
                std::string status = extras::safeFolderId(f.id) ? restGet(st, "/rest/db/status?folder=" + f.id) : "";
                extras::folderState(status, f.paused, e.state_, e.progress_);
            });
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::PauseJob:
    case O::ResumeJob: {
        shared::FileSyncActionRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.name_[sizeof(req.name_) - 1] = '\0';
        bool pause = static_cast<O>(msg.opcode_) == O::PauseJob;
        Syncthing st = loadSyncthing();
        bool ok = false;
        for (const auto& f : st.folders) {
            if (f.label != req.name_ || !extras::safeFolderId(f.id) || st.apikey.empty()) continue;
            int rc = spawnStatus("curl -sf -m 5 -X PATCH -H " + shellQuote("X-API-Key: " + st.apikey) +
                                 " -H 'Content-Type: application/json' -d " +
                                 shellQuote(pause ? "{\"paused\":true}" : "{\"paused\":false}") + " " +
                                 shellQuote("http://" + st.address + "/rest/config/folders/" + f.id) +
                                 " >/dev/null 2>&1");
            ok = rc == 0;
            break;
        }
        if (!ok) LOG_WARN("FileSync: can't {} '{}'{}", pause ? "pause" : "resume", req.name_,
                          st.present ? "" : " (Syncthing isn't set up)");
        ack(msg, client, kName);
        if (ok) broadcast<char>(kName, static_cast<uint32_t>(O::JobsChanged), nullptr);
        break;
    }
    default: break;
    }
}

// ── LiveCaptionsService ──────────────────────────────────────────────────────

void LiveCaptionsService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::LiveCaptionsOpcode;
    constexpr const char* kName = "com.freebsdx.LiveCaptions";
    auto state = [] {
        shared::LiveCaptionsState st{};
        st.enabled_   = 0;   // no speech-recognition engine
        st.position_  = std::min<uint32_t>(std::strtoul(ConfigService::get("captions.position", "0").c_str(), nullptr, 10), 1);
        st.font_size_ = std::min<uint32_t>(std::strtoul(ConfigService::get("captions.font_size", "1").c_str(), nullptr, 10), 2);
        return st;
    };
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState: {
        auto st = state();
        reply(msg, client, kName, msg.opcode_, &st);
        break;
    }
    case O::SetState: {
        shared::LiveCaptionsRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        ConfigService::set("captions.position", std::to_string(std::min<uint32_t>(req.position_, 1)));
        ConfigService::set("captions.font_size", std::to_string(std::min<uint32_t>(req.font_size_, 2)));
        if (req.enabled_) LOG_WARN("LiveCaptions: no speech-recognition engine is installed");
        ack(msg, client, kName);
        auto st = state();
        broadcast(kName, static_cast<uint32_t>(O::StateChanged), &st);
        break;
    }
    default: break;
    }
}

// ── CastService ──────────────────────────────────────────────────────────────

void CastService::handleMessage(const shared::XBusMessage& msg,
                                std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::CastOpcode;
    constexpr const char* kName = "com.freebsdx.Cast";
    switch (static_cast<O>(msg.opcode_)) {
    case O::Scan: {
        shared::CastReceiverPage pg{};   // no receivers: nothing can discover them
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::GetState: {
        shared::CastState st{};
        reply(msg, client, kName, msg.opcode_, &st);
        break;
    }
    case O::Connect:
        LOG_WARN("Cast: no Miracast/AirPlay sender is available on this system");
        break;
    case O::Disconnect:
        break;
    default: break;
    }
}

}  // namespace fbx::xserver
