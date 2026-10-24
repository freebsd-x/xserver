#include "xserver/screensaver_service.h"
#include "xserver/lock_service.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

// ── Static state ──────────────────────────────────────────────────────────────

std::atomic<bool>     ScreenSaverService::active_{false};
std::atomic<uint32_t> ScreenSaverService::timeout_seconds_{5 * 60}; // 5 min default
std::atomic<uint32_t> ScreenSaverService::mode_{0};                  // 0 = blank
std::atomic<uint32_t> ScreenSaverService::inhibit_count_{0};
std::function<bool()>  ScreenSaverService::wake_inhibitor_;
pid_t                 ScreenSaverService::saver_pid_{-1};
std::chrono::steady_clock::time_point ScreenSaverService::last_input_ =
    std::chrono::steady_clock::now();

// ── Internal helpers ──────────────────────────────────────────────────────────

bool ScreenSaverService::isActive()      noexcept { return active_.load(); }
uint32_t ScreenSaverService::timeoutSeconds() noexcept { return timeout_seconds_.load(); }

void ScreenSaverService::broadcastState() noexcept {
    shared::XBusMessage msg{};
    msg.magic_ = shared::XBUS_MAGIC;
    std::strncpy(msg.service_name_, "com.freebsdx.ScreenSaver",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::ScreenSaverOpcode::StateChanged);

    shared::ScreenSaverState st{};
    st.active_       = active_.load() ? 1u : 0u;
    st.enabled_      = (timeout_seconds_.load() > 0) ? 1u : 0u;
    st.inhibited_    = (inhibit_count_.load() > 0) ? 1u : 0u;
    st.timeout_secs_ = timeout_seconds_.load();
    st.mode_         = mode_.load();
    std::memcpy(msg.payload_, &st, sizeof(st));
    BroadcastXBusMessage(msg);
}

static const char* modeArg(uint32_t m) noexcept {
    switch (m) {
    case 1:  return "clock";
    case 2:  return "matrix";
    default: return "blank";
    }
}

void ScreenSaverService::activate() noexcept {
    if (active_.load()) return;
    active_.store(true);
    std::cout << "[ScreenSaverService] Activating (mode=" << mode_.load() << ")\n";
    broadcastState();

    pid_t pid = ::fork();
    if (pid == 0) {
        ::setenv("XBUS_SOCKET", "/tmp/freebsdx.sock", 1);
        const char* mode_str = modeArg(mode_.load());
        if (::access("./out/screensaver", X_OK) == 0)
            ::execl("./out/screensaver", "screensaver", mode_str, nullptr);
        else if (::access("/usr/local/bin/freebsdx-screensaver", X_OK) == 0)
            ::execl("/usr/local/bin/freebsdx-screensaver",
                    "freebsdx-screensaver", mode_str, nullptr);
        ::_exit(1);
    }
    if (pid > 0)
        saver_pid_ = pid;
}

void ScreenSaverService::deactivate() noexcept {
    if (!active_.load()) return;
    active_.store(false);
    std::cout << "[ScreenSaverService] Deactivating\n";

    if (saver_pid_ > 0) {
        ::kill(saver_pid_, SIGTERM);
        ::waitpid(saver_pid_, nullptr, WNOHANG);
        saver_pid_ = -1;
    }
    broadcastState();
}

// ── Public API ────────────────────────────────────────────────────────────────

void ScreenSaverService::resetIdleTimer() noexcept {
    last_input_ = std::chrono::steady_clock::now();
    if (active_.load()) {
        deactivate();
        // If the lock timeout also expired while the saver was showing,
        // hand off to LockService — the saver was masking the lock.
        if (!LockService::isLocked()) {
            uint32_t lock_to = LockService::timeoutSeconds();
            if (lock_to > 0) {
                // last_input_ was just reset, but the real elapsed time before
                // the input is what matters. Use saver activation as a proxy:
                // if the lock timeout ≤ screensaver timeout, lock was overdue.
                if (lock_to <= timeout_seconds_.load())
                    LockService::lock();
            }
        }
    }
}

void ScreenSaverService::setWakeInhibitor(std::function<bool()> pred) noexcept {
    wake_inhibitor_ = std::move(pred);
}

void ScreenSaverService::pollIdle() noexcept {
    uint32_t timeout = timeout_seconds_.load();
    // A keep-awake (WakeLock) holds the idle clock reset so the saver never fires.
    if (wake_inhibitor_ && wake_inhibitor_()) { resetIdleTimer(); return; }
    // Skip if disabled, already active, inhibited, or lock screen is up.
    if (timeout == 0 || active_.load() ||
        inhibit_count_.load() > 0 || LockService::isLocked())
        return;

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - last_input_).count();
    if (static_cast<uint32_t>(elapsed) >= timeout)
        activate();
}

// ── ProcessMessage ────────────────────────────────────────────────────────────

bool ScreenSaverService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                        const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.ScreenSaver") != 0) return false;

    using namespace shared;
    XBusMessage response = msg;

    switch (static_cast<ScreenSaverOpcode>(msg.opcode_)) {

    case ScreenSaverOpcode::GetState: {
        ScreenSaverState st{};
        st.active_       = active_.load() ? 1u : 0u;
        st.enabled_      = (timeout_seconds_.load() > 0) ? 1u : 0u;
        st.inhibited_    = (inhibit_count_.load() > 0) ? 1u : 0u;
        st.timeout_secs_ = timeout_seconds_.load();
        st.mode_         = mode_.load();
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case ScreenSaverOpcode::SetConfig: {
        ScreenSaverConfig cfg{};
        std::memcpy(&cfg, msg.payload_, sizeof(cfg));
        timeout_seconds_.store(cfg.timeout_secs_);
        mode_.store(cfg.mode_);
        std::cout << "[ScreenSaverService] Config updated: timeout="
                  << cfg.timeout_secs_ << "s mode=" << cfg.mode_ << "\n";
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        broadcastState();
        break;
    }

    case ScreenSaverOpcode::GetConfig: {
        ScreenSaverConfig cfg{};
        cfg.timeout_secs_ = timeout_seconds_.load();
        cfg.mode_         = mode_.load();
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &cfg, sizeof(cfg));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case ScreenSaverOpcode::Inhibit: {
        inhibit_count_.fetch_add(1);
        std::cout << "[ScreenSaverService] Inhibited (count=" << inhibit_count_.load() << ")\n";
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case ScreenSaverOpcode::Uninhibit: {
        if (inhibit_count_.load() > 0)
            inhibit_count_.fetch_sub(1);
        std::cout << "[ScreenSaverService] Uninhibited (count=" << inhibit_count_.load() << ")\n";
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case ScreenSaverOpcode::Register: {
        // screensaver announces itself — pid already stored by activate().
        std::cout << "[ScreenSaverService] Screensaver app registered.\n";
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
