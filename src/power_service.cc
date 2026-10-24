#include "xserver/power_service.h"
#include "xserver/async_shell.h"
#include "xserver/lock_service.h"
#include "xserver/power_profile_service.h"
#include "xserver/ipc_socket.h"
#include <sys/types.h>
#include <sys/sysctl.h>
#include <cstring>
#include <cstdlib>
#include <iostream>

// Forward-declared in main.cc — broadcasts a message to every connected client.
namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

// ── Static state ────────────────────────────────────────────────────────────

shared::PowerLidAction PowerService::lid_policy_           = shared::PowerLidAction::Suspend;
uint32_t               PowerService::sleep_timeout_seconds_ = 15 * 60; // 15 min default
uint32_t               PowerService::last_on_ac_            = 0xFF;    // uninitialised

// ── ACPI helpers ─────────────────────────────────────────────────────────────

shared::PowerState PowerService::getPowerState() noexcept {
    shared::PowerState st{};
    st.time_remaining_ = 0xFFFFFFFFu; // unknown until sysctl succeeds

    // hw.acpi.acline: 0 = on battery, 1 = on AC
    int acline = 1;
    size_t len = sizeof(acline);
    if (sysctlbyname("hw.acpi.acline", &acline, &len, nullptr, 0) == 0)
        st.on_ac_ = static_cast<uint32_t>(acline);
    else
        st.on_ac_ = 1; // assume AC if sysctl missing (desktop)

    // hw.acpi.battery.life: 0–100
    int life = 100;
    len = sizeof(life);
    if (sysctlbyname("hw.acpi.battery.life", &life, &len, nullptr, 0) == 0)
        st.battery_life_ = static_cast<uint32_t>(life);
    else
        st.battery_life_ = 100;

    // hw.acpi.battery.state: 0=high/discharging, 1=low, 2=critical,
    //   4=charging, 7=absent. Map to protocol: 0=discharging,1=charging,
    //   2=full,3=absent.
    int bstate = 0;
    len = sizeof(bstate);
    if (sysctlbyname("hw.acpi.battery.state", &bstate, &len, nullptr, 0) == 0) {
        if (bstate == 7)
            st.battery_state_ = 3; // absent
        else if (bstate == 4)
            st.battery_state_ = 1; // charging
        else if (life >= 99 && acline)
            st.battery_state_ = 2; // full
        else
            st.battery_state_ = 0; // discharging
    } else {
        st.battery_state_ = 2; // assume full on desktop
    }

    // hw.acpi.battery.time: minutes remaining; -1 if unknown/charging
    int btime = -1;
    len = sizeof(btime);
    if (sysctlbyname("hw.acpi.battery.time", &btime, &len, nullptr, 0) == 0 && btime >= 0)
        st.time_remaining_ = static_cast<uint32_t>(btime);

    return st;
}

// ── Poll for AC/battery change and broadcast ──────────────────────────────────

void PowerService::pollAndBroadcast() noexcept {
    int acline = 1;
    size_t len = sizeof(acline);
    if (sysctlbyname("hw.acpi.acline", &acline, &len, nullptr, 0) != 0)
        return; // no ACPI (desktop without battery — skip)

    uint32_t on_ac = static_cast<uint32_t>(acline);
    if (on_ac == last_on_ac_) return; // no change
    last_on_ac_ = on_ac;

    std::cout << "[PowerService] AC state changed: " << (on_ac ? "plugged in" : "on battery") << "\n";

    PowerProfileService::onAcChanged(on_ac != 0);

    shared::XBusMessage msg{};
    msg.magic_ = shared::XBUS_MAGIC;
    std::strncpy(msg.service_name_, "com.freebsdx.Power", sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::PowerOpcode::PowerChanged);
    shared::PowerState st = getPowerState();
    std::memcpy(msg.payload_, &st, sizeof(st));
    BroadcastXBusMessage(msg);
}

// ── ProcessMessage ────────────────────────────────────────────────────────────

void PowerService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                  const shared::XBusMessage& msg) noexcept {
    using namespace shared;
    XBusMessage resp = msg;

    switch (static_cast<PowerOpcode>(msg.opcode_)) {

    case PowerOpcode::GetPowerState: {
        PowerState st = getPowerState();
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &st, sizeof(st));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case PowerOpcode::Suspend:
        std::cout << "[PowerService] Suspend requested\n";
        LockService::lock(); // lock before suspending
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        (void)fbx::xserver::spawnStatus("acpiconf -s 3");
        break;

    case PowerOpcode::Hibernate:
        std::cout << "[PowerService] Hibernate requested\n";
        LockService::lock(); // lock before hibernating
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        (void)fbx::xserver::spawnStatus("acpiconf -s 4");
        break;

    case PowerOpcode::Reboot:
        std::cout << "[PowerService] Reboot requested\n";
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        (void)fbx::xserver::spawnStatus("shutdown -r now");
        break;

    case PowerOpcode::SetLidPolicy: {
        PowerLidPolicyRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        lid_policy_ = static_cast<PowerLidAction>(req.action_);
        std::cout << "[PowerService] Lid policy set to " << req.action_ << "\n";
        // Reflect to ACPI if possible (best-effort; may require root)
        const char* states[] = {"none", "S3", "S4", "S5"};
        if (req.action_ < 4) {
            std::string cmd = std::string("sysctl hw.acpi.lid_switch_state=")
                              + states[req.action_];
            (void)fbx::xserver::spawnStatus(cmd.c_str());
        }
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case PowerOpcode::GetLidPolicy: {
        PowerLidPolicyRequest rep{};
        rep.action_ = static_cast<uint32_t>(lid_policy_);
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &rep, sizeof(rep));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case PowerOpcode::SetSleepTimeout: {
        PowerSleepTimeoutRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        sleep_timeout_seconds_ = req.seconds_;
        std::cout << "[PowerService] Sleep timeout set to " << req.seconds_ << "s\n";
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case PowerOpcode::GetSleepTimeout: {
        PowerSleepTimeoutRequest rep{};
        rep.seconds_ = sleep_timeout_seconds_;
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &rep, sizeof(rep));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    default:
        std::cerr << "[PowerService] Unknown opcode: 0x"
                  << std::hex << msg.opcode_ << "\n";
        break;
    }
}

shared::PowerLidAction PowerService::getLidPolicy() noexcept { return lid_policy_; }
uint32_t PowerService::getSleepTimeoutSeconds() noexcept { return sleep_timeout_seconds_; }

} // namespace fbx::xserver
