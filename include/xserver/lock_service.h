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

// LockService — idle-lock / screensaver for FreeBSD-X
//
// Tracks the idle timer, spawns lockscreen when the screen should lock,
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

    // Returns the IpcSocket of the registered lock screen app, or nullptr.
    [[nodiscard]] static std::shared_ptr<IpcSocket> lockscreenClient() noexcept;

    // Reset idle timer — call on every user input event.
    static void resetIdleTimer() noexcept;

    // Check idle timeout and lock if exceeded — call from event-loop timer (~10 s).
    static void pollIdle() noexcept;

    // A predicate consulted in pollIdle; while true, the idle-lock clock is held
    // reset (wired to WakeLock keep-awake).
    static void setWakeInhibitor(std::function<bool()> pred) noexcept;

private:
    static void broadcastState() noexcept;
    static std::function<bool()> wake_inhibitor_;
    static bool pamAuthenticate(const char* user, const char* password) noexcept;
    static void spawn_lockscreen() noexcept;

    static std::atomic<bool>     locked_;
    static std::atomic<uint32_t> timeout_seconds_; // 0 = never
    static std::mutex            state_mutex_;
    static std::weak_ptr<IpcSocket> lockscreen_client_;
    static pid_t                 lockscreen_pid_;
    static std::chrono::steady_clock::time_point last_input_;
};

} // namespace fbx::xserver
