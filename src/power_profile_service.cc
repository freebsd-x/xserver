#include "xserver/power_profile_service.h"
#include "xserver/ipc_socket.h"
#include <sys/sysctl.h>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

// ── Static state ─────────────────────────────────────────────────────────────

shared::PowerProfileLevel PowerProfileService::current_    = shared::PowerProfileLevel::Balanced;
uint32_t                  PowerProfileService::cpu_min_mhz_ = 0;
uint32_t                  PowerProfileService::cpu_max_mhz_ = 0;
uint32_t                  PowerProfileService::cpu_cur_mhz_ = 0;

// ── CPU frequency helpers ────────────────────────────────────────────────────

void PowerProfileService::readCpuFreqRange() noexcept {
    // Read dev.cpu.0.freq_levels — space-separated "freq/power" pairs
    char buf[256] = {};
    size_t len = sizeof(buf) - 1;
    if (sysctlbyname("dev.cpu.0.freq_levels", buf, &len, nullptr, 0) != 0) {
        cpu_min_mhz_ = 0;
        cpu_max_mhz_ = 0;
        return;
    }

    std::istringstream ss(buf);
    std::string token;
    uint32_t first_freq = 0, last_freq = 0;
    bool any = false;
    while (ss >> token) {
        auto slash = token.find('/');
        if (slash == std::string::npos) continue;
        uint32_t freq = static_cast<uint32_t>(std::strtoul(token.c_str(), nullptr, 10));
        if (!any) { first_freq = freq; any = true; }
        last_freq = freq;
    }
    if (any) {
        // freq_levels is ordered highest-to-lowest
        cpu_max_mhz_ = first_freq;
        cpu_min_mhz_ = last_freq;
    }

    // Read current frequency
    int cur = 0;
    len = sizeof(cur);
    if (sysctlbyname("dev.cpu.0.freq", &cur, &len, nullptr, 0) == 0)
        cpu_cur_mhz_ = static_cast<uint32_t>(cur);
}

void PowerProfileService::init() noexcept {
    readCpuFreqRange();
    std::cout << "[PowerProfileService] init: min=" << cpu_min_mhz_
              << " max=" << cpu_max_mhz_ << " cur=" << cpu_cur_mhz_ << " MHz\n";
}

// ── Apply profile ────────────────────────────────────────────────────────────

shared::PowerProfileLevel PowerProfileService::applyProfile(
    shared::PowerProfileLevel level) noexcept
{
    using L = shared::PowerProfileLevel;

    // Determine target frequency
    uint32_t target_freq = 0;
    const char* cx_state = "C1";

    switch (level) {
    case L::Performance:
        target_freq = cpu_max_mhz_;
        cx_state    = "C1";
        break;
    case L::Balanced:
        target_freq = (cpu_max_mhz_ + cpu_min_mhz_) / 2;
        cx_state    = "C2";
        break;
    case L::PowerSaver:
        target_freq = cpu_min_mhz_;
        cx_state    = "C3";
        break;
    case L::Emergency:
        target_freq = cpu_min_mhz_;
        cx_state    = "C3";
        break;
    }

    // Set cx_lowest
    if (cx_state[0]) {
        sysctlbyname("hw.acpi.cpu.cx_lowest",
                     nullptr, nullptr,
                     cx_state, strlen(cx_state));
    }

    // Set freq on all CPUs
    if (target_freq > 0) {
        int ncpu = 1;
        size_t len = sizeof(ncpu);
        (void)sysctlbyname("hw.ncpu", &ncpu, &len, nullptr, 0);
        for (int i = 0; i < ncpu; ++i) {
            char oid[64];
            std::snprintf(oid, sizeof(oid), "dev.cpu.%d.freq", i);
            int freq = static_cast<int>(target_freq);
            (void)sysctlbyname(oid, nullptr, nullptr, &freq, sizeof(freq));
        }
        cpu_cur_mhz_ = target_freq;
    }

    // For Emergency: dim backlight via hw.acpi.video.lcd0.brightness = 50
    if (level == L::Emergency) {
        int bright = 50;
        (void)sysctlbyname("hw.acpi.video.lcd0.brightness",
                           nullptr, nullptr, &bright, sizeof(bright));
    }

    current_ = level;

    // Broadcast ProfileChanged
    shared::XBusMessage msg{};
    msg.magic_ = shared::XBUS_MAGIC;
    std::strncpy(msg.service_name_, "com.freebsdx.PowerProfile",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::PowerProfileOpcode::ProfileChanged);
    shared::PowerProfileState st = getState();
    std::memcpy(msg.payload_, &st, sizeof(st));
    BroadcastXBusMessage(msg);

    return current_;
}

// ── State helper ─────────────────────────────────────────────────────────────

shared::PowerProfileState PowerProfileService::getState() noexcept {
    // Refresh current freq
    int cur = 0;
    size_t len = sizeof(cur);
    if (sysctlbyname("dev.cpu.0.freq", &cur, &len, nullptr, 0) == 0)
        cpu_cur_mhz_ = static_cast<uint32_t>(cur);

    shared::PowerProfileState st{};
    st.profile_      = static_cast<uint32_t>(current_);
    st.cpu_freq_mhz_ = cpu_cur_mhz_;
    st.cpu_max_mhz_  = cpu_max_mhz_;
    st.cpu_min_mhz_  = cpu_min_mhz_;
    return st;
}

// ── onAcChanged ──────────────────────────────────────────────────────────────

void PowerProfileService::onAcChanged(bool on_ac) noexcept {
    using L = shared::PowerProfileLevel;
    L new_level = on_ac ? L::Balanced : L::PowerSaver;
    std::cout << "[PowerProfileService] AC changed → "
              << (on_ac ? "Balanced" : "PowerSaver") << "\n";
    applyProfile(new_level);
}

// ── ProcessMessage ────────────────────────────────────────────────────────────

bool PowerProfileService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                         const shared::XBusMessage& msg) noexcept
{
    using namespace shared;
    XBusMessage resp = msg;

    switch (static_cast<PowerProfileOpcode>(msg.opcode_)) {

    case PowerProfileOpcode::GetProfile: {
        PowerProfileState st = getState();
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        return true;
    }

    case PowerProfileOpcode::SetProfile: {
        PowerProfileRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        auto level = static_cast<PowerProfileLevel>(req.profile_);
        applyProfile(level);
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        return true;
    }

    case PowerProfileOpcode::ListProfiles: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));

        // 4 profiles total
        if (page < 4) {
            static const struct {
                PowerProfileLevel level;
                const char* name;
                const char* desc;
            } kProfiles[4] = {
                { PowerProfileLevel::Performance, "Performance",
                  "Maximum CPU frequency, C1 idle state" },
                { PowerProfileLevel::Balanced, "Balanced",
                  "Mid CPU frequency, C2 idle state" },
                { PowerProfileLevel::PowerSaver, "Power Saver",
                  "Minimum CPU frequency, C3 idle state" },
                { PowerProfileLevel::Emergency, "Emergency",
                  "Minimum CPU frequency, C3, dim display" },
            };

            PowerProfilePage pp{};
            pp.page_    = page;
            pp.total_   = 4;
            pp.profile_ = static_cast<uint32_t>(kProfiles[page].level);
            std::strncpy(pp.name_,        kProfiles[page].name, sizeof(pp.name_) - 1);
            std::strncpy(pp.description_, kProfiles[page].desc, sizeof(pp.description_) - 1);

            resp.opcode_ = static_cast<uint32_t>(PowerProfileOpcode::ProfilePage);
            std::memset(resp.payload_, 0, sizeof(resp.payload_));
            std::memcpy(resp.payload_, &pp, sizeof(pp));
            (void)client->sendMessage(
                {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        }
        return true;
    }

    default:
        std::cerr << "[PowerProfileService] Unknown opcode: 0x"
                  << std::hex << msg.opcode_ << "\n";
        return false;
    }
}

} // namespace fbx::xserver
