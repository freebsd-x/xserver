#include "xserver/lock_service.h"
#include "xserver/auth_helper.h"
#include "xserver/auth_throttle.h"
#include "xserver/security_events_service.h"
#include <chrono>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

// ── Static state ──────────────────────────────────────────────────────────────

std::atomic<bool>     LockService::locked_{false};
std::function<bool()>  LockService::wake_inhibitor_;
std::atomic<uint32_t> LockService::timeout_seconds_{10 * 60}; // 10 min default
std::mutex            LockService::state_mutex_;
std::weak_ptr<IpcSocket> LockService::lockscreen_client_;
pid_t                 LockService::lockscreen_pid_{-1};
std::chrono::steady_clock::time_point LockService::last_input_ =
    std::chrono::steady_clock::now();

bool LockService::pamAuthenticate(const char* user, const char* password) noexcept {
    // Determine current user if caller passed empty string
    if (!user || user[0] == '\0') {
        user = ::getenv("USER");
        if (!user) user = "root";
    }
    // Shared full chain (xsessiond → setuid helper → in-process PAM w/
    // pam_acct_mgmt) — identical policy to the login greeter. See auth_helper.h.
    return pamAuthenticateFull("freebsdx", user, password);
}

// ── Spawn / kill lockscreen ───────────────────────────────────────────────────

void LockService::spawn_lockscreen() noexcept {
    pid_t pid = ::fork();
    if (pid == 0) {
        ::setenv("LD_LIBRARY_PATH", "./out", 1);
        ::setenv("XBUS_SOCKET", "/tmp/freebsdx.sock", 1);
        if (::access("./out/lockscreen", X_OK) == 0)
            ::execl("./out/lockscreen", "lockscreen", nullptr);
        else if (::access("/usr/local/bin/lockscreen", X_OK) == 0)
            ::execl("/usr/local/bin/lockscreen", "lockscreen", nullptr);
        ::exit(1);
    }
    lockscreen_pid_ = pid;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool LockService::isLocked() noexcept { return locked_.load(); }

uint32_t LockService::timeoutSeconds() noexcept { return timeout_seconds_.load(); }
void LockService::setTimeoutSeconds(uint32_t seconds) noexcept { timeout_seconds_.store(seconds); }

std::shared_ptr<IpcSocket> LockService::lockscreenClient() noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return lockscreen_client_.lock();
}

void LockService::lock() noexcept {
    if (locked_.load()) return;
    locked_.store(true);
    std::cout << "[LockService] Screen locked.\n";
    broadcastState();
    spawn_lockscreen();
}

void LockService::unlock() noexcept {
    if (!locked_.load()) return;
    locked_.store(false);
    std::cout << "[LockService] Screen unlocked.\n";

    // Kill the lock screen process
    if (lockscreen_pid_ > 0) {
        ::kill(lockscreen_pid_, SIGTERM);
        // Reap zombie asynchronously
        ::waitpid(lockscreen_pid_, nullptr, WNOHANG);
        lockscreen_pid_ = -1;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        lockscreen_client_.reset();
    }
    broadcastState();
    resetIdleTimer();
}

void LockService::resetIdleTimer() noexcept {
    last_input_ = std::chrono::steady_clock::now();
}

void LockService::setWakeInhibitor(std::function<bool()> pred) noexcept {
    wake_inhibitor_ = std::move(pred);
}

void LockService::pollIdle() noexcept {
    uint32_t timeout = timeout_seconds_.load();
    if (timeout == 0 || locked_.load()) return;
    // A keep-awake (WakeLock) holds the idle-lock clock reset.
    if (wake_inhibitor_ && wake_inhibitor_()) { resetIdleTimer(); return; }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - last_input_).count();
    if (static_cast<uint32_t>(elapsed) >= timeout) {
        lock();
    }
}

// ── Broadcast ─────────────────────────────────────────────────────────────────

void LockService::broadcastState() noexcept {
    shared::XBusMessage msg{};
    msg.magic_ = shared::XBUS_MAGIC;
    std::strncpy(msg.service_name_, "com.freebsdx.Lock",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::LockOpcode::LockStateChanged);

    shared::LockState st{};
    st.locked_       = locked_.load() ? 1u : 0u;
    st.timeout_secs_ = timeout_seconds_.load();
    std::memcpy(msg.payload_, &st, sizeof(st));
    BroadcastXBusMessage(msg);
}

// ── ProcessMessage ────────────────────────────────────────────────────────────

bool LockService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                  const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.Lock") != 0) return false;

    using namespace shared;
    XBusMessage response = msg;

    switch (static_cast<LockOpcode>(msg.opcode_)) {

    case LockOpcode::Lock: {
        lock();
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case LockOpcode::Unlock: {
        LockAuthRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.username_[sizeof(req.username_) - 1] = '\0';
        req.password_[sizeof(req.password_) - 1] = '\0';

        // Same brute-force throttle as the login greeter (shared instance), so a
        // lockout can't be sidestepped by attacking the lock screen instead.
        auto& throttle = AuthThrottle::global();
        const auto now = std::chrono::steady_clock::now();
        auto gate = throttle.check(req.username_, now);
        bool ok = false;
        if (gate.allowed) {
            ok = pamAuthenticate(req.username_, req.password_);
            if (ok) throttle.recordSuccess(req.username_);
            else    throttle.recordFailure(req.username_, now);
            SecurityEventsService::record(ok ? "unlock" : "fail", req.username_, "local", ok);
        } else {
            std::cerr << "[LockService] throttled unlock for " << req.username_
                      << " — retry in " << gate.retry_after_s << "s\n";
            SecurityEventsService::record(gate.locked_out ? "lockout" : "throttle",
                                          req.username_, "local", false);
        }
        // Immediately zero the password from memory
        ::explicit_bzero(req.password_, sizeof(req.password_));

        LockState st{};
        st.timeout_secs_ = timeout_seconds_.load();
        if (ok) {
            unlock();
            st.locked_ = 0;
        } else {
            st.locked_ = locked_.load() ? 1u : 0u;
            std::cerr << "[LockService] Authentication failed.\n";
        }
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case LockOpcode::GetState: {
        LockState st{};
        st.locked_       = locked_.load() ? 1u : 0u;
        st.timeout_secs_ = timeout_seconds_.load();
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case LockOpcode::SetTimeout: {
        LockTimeoutRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        timeout_seconds_.store(req.seconds_);
        std::cout << "[LockService] Idle timeout set to " << req.seconds_ << " s\n";
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case LockOpcode::GetTimeout: {
        LockTimeoutRequest rep{};
        rep.seconds_ = timeout_seconds_.load();
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &rep, sizeof(rep));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case LockOpcode::Register: {
        // lockscreen identifies itself so the compositor can route
        // exclusive input to it while locked.
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            lockscreen_client_ = client;
        }
        std::cout << "[LockService] Lock screen registered.\n";
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
