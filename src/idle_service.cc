#include "xserver/idle_service.h"
#include "xserver/config_service.h"
#include "xserver/lock_service.h"
#include "xserver/screen_timeout_service.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace fbx::xserver {

namespace {
using Clock = std::chrono::steady_clock;

IdleService::Hooks g_hooks;
Clock::time_point g_last_input = Clock::now();
Clock::time_point g_locked_at;
bool g_was_locked = false;
idle::Phase g_phase = idle::Phase::Active;
// Who holds idle off: each client once, however often it asks, and not beyond its end.
std::vector<std::weak_ptr<IpcSocket>> g_holders;

bool sameClient(const std::weak_ptr<IpcSocket>& a, const std::shared_ptr<IpcSocket>& b) {
    return a.lock() == b;
}

uint32_t secondsSince(Clock::time_point t) {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t).count());
}

void setPhase(idle::Phase p) {
    if (p == g_phase) return;
    g_phase = p;
    static constexpr const char* kNames[] = {"on", "dimmed", "off"};
    std::cout << "[Idle] Screen " << kNames[static_cast<int>(p)] << "\n" << std::flush;
    if (g_hooks.set_phase) g_hooks.set_phase(p);
}

bool holding() {
    std::erase_if(g_holders, [](const auto& w) { return w.expired(); });
    return !g_holders.empty();
}

void reply(const shared::XBusMessage& msg, const std::shared_ptr<IpcSocket>& client) {
    if (!client) return;
    shared::IdleState st{};
    st.held_ = holding() || (g_hooks.held && g_hooks.held()) ? 1u : 0u;
    st.phase_ = static_cast<uint32_t>(g_phase);
    shared::XBusMessage m = msg;
    std::memset(m.payload_, 0, sizeof(m.payload_));
    std::memcpy(m.payload_, &st, sizeof(st));
    (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
}  // namespace

void IdleService::init(Hooks hooks) noexcept { g_hooks = std::move(hooks); }

idle::Phase IdleService::phase() noexcept { return g_phase; }

bool IdleService::noteInput() noexcept {
    g_last_input = Clock::now();
    const bool woke = g_phase == idle::Phase::Off;
    setPhase(idle::Phase::Active);
    return woke;
}

void IdleService::onClientDisconnect(const std::shared_ptr<IpcSocket>& client) noexcept {
    std::erase_if(g_holders, [&](const auto& w) { return w.expired() || sameClient(w, client); });
}

void IdleService::tick() noexcept {
    const bool locked = LockService::isLocked();
    if (locked && !g_was_locked) g_locked_at = Clock::now();
    g_was_locked = locked;

    const bool held = holding() || (g_hooks.held && g_hooks.held());
    // Held until now: idle starts again from here, not from the last key.
    if (held && !locked) g_last_input = Clock::now();

    idle::Settings s;
    s.lock_secs = LockService::timeoutSeconds();
    s.off_secs = ScreenTimeoutService::seconds();
    s.locked_off_secs = static_cast<uint32_t>(std::strtoul(ConfigService::get("idle.locked_off_secs", "30").c_str(), nullptr, 10));
    idle::Now n;
    n.idle_secs = secondsSince(g_last_input);
    n.locked = locked;
    n.locked_secs = locked ? secondsSince(g_locked_at) : 0;
    n.held = held;
    n.at_greeter = g_hooks.at_greeter && g_hooks.at_greeter();

    const idle::Decision d = idle::decide(s, n);
    if (d.lock && !locked) {
        LockService::lock();
        g_locked_at = Clock::now();
        g_was_locked = true;
    }
    setPhase(d.phase);
}

void IdleService::handleMessage(const shared::XBusMessage& msg, std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::IdleOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::Hold:
        if (client && std::none_of(g_holders.begin(), g_holders.end(), [&](const auto& w) { return sameClient(w, client); })) {
            g_holders.push_back(client);
            std::cout << "[Idle] Held off by a client\n" << std::flush;
        }
        reply(msg, client);
        break;
    case O::Release:
        std::erase_if(g_holders, [&](const auto& w) { return sameClient(w, client); });
        reply(msg, client);
        break;
    case O::GetState:
        reply(msg, client);
        break;
    default: break;
    }
}

}  // namespace fbx::xserver
