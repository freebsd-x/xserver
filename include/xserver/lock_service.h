#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <atomic>
#include <functional>
#include <mutex>
#include <chrono>
#include <sys/types.h>

namespace fbx::xserver {

// LockService — the lock screen. (When: IdleService, Super+L, sleep.)
//
// Spawns lockscreen when the screen should lock,
// enforces exclusive input routing while locked, and authenticates via PAM.
class LockService {
public:
    // XBus message handler for com.freebsdx.Lock
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    // Initiate a screen lock (spawns lockscreen, broadcasts LockStateChanged).
    static void lock() noexcept;

    // Clear the locked state (called after successful PAM auth).
    static void unlock() noexcept;

    [[nodiscard]] static bool     isLocked()      noexcept;
    [[nodiscard]] static uint32_t timeoutSeconds() noexcept;
    // Idle auto-lock timeout; 0 disables it. (com.freebsdx.ScreenLock policy.)
    static void setTimeoutSeconds(uint32_t seconds) noexcept;

    // Returns the IpcSocket of the registered lock screen app, or nullptr.
    [[nodiscard]] static std::shared_ptr<IpcSocket> lockscreenClient() noexcept;

    // Once a second: starts the lock screen again if it died while locked.
    static void tick() noexcept;

private:
    static void broadcastState() noexcept;
    static bool pamAuthenticate(const char* user, const char* password) noexcept;
    static void spawn_lockscreen() noexcept;

    static std::atomic<bool>     locked_;
    static std::atomic<uint32_t> timeout_seconds_; // 0 = never
    static std::mutex            state_mutex_;
    static std::weak_ptr<IpcSocket> lockscreen_client_;
    static pid_t                 lockscreen_pid_;
};

} // namespace fbx::xserver
