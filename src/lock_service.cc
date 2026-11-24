#include "xserver/lock_service.h"
#include "xserver/spawn_util.h"
#include "xserver/auth_helper.h"
#include "xserver/config_service.h"
#include "xserver/peer_identity.h"
#include "xserver/auth_throttle.h"
#include "xserver/security_events_service.h"
#include <chrono>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

// ── Static state ──────────────────────────────────────────────────────────────

std::atomic<bool>     LockService::locked_{false};
std::atomic<uint32_t> LockService::timeout_seconds_{10 * 60}; // 10 min default
std::mutex            LockService::state_mutex_;
std::weak_ptr<IpcSocket> LockService::lockscreen_client_;
pid_t                 LockService::lockscreen_pid_{-1};

// Whose session this is: the account the compositor (and all its apps) run as. Only
// that account's password opens its screen, whatever name the lock screen sends.
static std::string sessionUser() {
    const passwd* pw = ::getpwuid(::geteuid());
    return pw && pw->pw_name ? pw->pw_name : "";
}

bool LockService::pamAuthenticate(const char* user, const char* password) noexcept {
    // Shared full chain (xsessiond → in-process PAM w/ pam_acct_mgmt) — identical policy to the login greeter. See auth_helper.h.
    return pamAuthenticateFull("freebsdx", user, password);
}

// ── Spawn / kill lockscreen ───────────────────────────────────────────────────

void LockService::spawn_lockscreen() noexcept {
    // posix_spawn, not fork + setenv (unsafe after fork in this multi-threaded
    // process; it could deadlock and leave the screen unlocked — spawn_util.h).
    const pid_t pid = spawn::start(
        spawn::firstExecutable({"./out/lockscreen", "/usr/local/bin/lockscreen"}), "lockscreen", {},
        {.env = {{"LD_LIBRARY_PATH", "./out"}, {"XBUS_SOCKET", "/tmp/freebsdx.sock"}}});
    if (pid < 0) std::cerr << "[LockService] couldn't start the lock screen\n";
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
}

// The lock screen is what stands between the session and whoever's at the keyboard: if
// it's gone while the screen is locked, another is started (the compositor shows
// nothing and delivers nothing meanwhile).
void LockService::tick() noexcept {
    if (!locked_.load()) return;
    if (lockscreen_pid_ > 0) {
        const pid_t r = ::waitpid(lockscreen_pid_, nullptr, WNOHANG);
        if (r == 0) return;                                      // running
        if (r < 0 && ::kill(lockscreen_pid_, 0) == 0) return;    // (someone else's to reap)
    }
    std::cerr << "[LockService] The lock screen is gone; starting another\n";
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        lockscreen_client_.reset();
    }
    spawn_lockscreen();
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
        // Only the lock screen asks: anything else would be guessing passwords.
        if (!client || client != lockscreenClient()) {
            std::cerr << "[LockService] DENIED: Unlock from something that isn't the lock screen\n";
            LockState no{};
            no.locked_ = locked_.load() ? 1u : 0u;
            no.timeout_secs_ = timeout_seconds_.load();
            std::memset(response.payload_, 0, sizeof(response.payload_));
            std::memcpy(response.payload_, &no, sizeof(no));
            if (client) (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
            break;
        }
        LockAuthRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.password_[sizeof(req.password_) - 1] = '\0';
        const std::string user = sessionUser();
        std::memset(req.username_, 0, sizeof(req.username_));
        std::strncpy(req.username_, user.c_str(), sizeof(req.username_) - 1);

        // Same brute-force throttle as the login greeter (shared instance), so a
        // lockout can't be sidestepped by attacking the lock screen instead.
        auto& throttle = AuthThrottle::global();
        const auto now = std::chrono::steady_clock::now();
        auto gate = throttle.check(req.username_, now);
        bool ok = false;
        if (gate.allowed) {
            ok = !user.empty() && pamAuthenticate(req.username_, req.password_);
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
        // Kept, as ScreenLock's policy is.
        ConfigService::set("screenlock.enabled", req.seconds_ ? "1" : "0");
        if (req.seconds_) ConfigService::set("screenlock.timeout", std::to_string(req.seconds_));
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
        // The lock screen says it's there: it gets all input while the screen is locked,
        // so nothing else may (it would be handed the password).
        if (!client || !peer::isOneOf(client->fd(), {"lockscreen"})) {
            std::cerr << "[LockService] DENIED: Register from something that isn't the lock screen\n";
            break;
        }
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
