#pragma once
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <memory>
#include <atomic>
#include <functional>
#include <chrono>
#include <sys/types.h>

namespace fbx::xserver {

// ScreenSaverService — compositor-side screen saver lifecycle manager.
//
// Separate from LockService: the saver fires at a shorter idle timeout
// (default 5 min) and shows a blank/clock/matrix overlay. Any user input
// dismisses it; if the lock timeout also expired during the saver, it
// hands off to LockService::lock() immediately on dismissal.
//
// Thread-safety: all methods called only from the single-threaded event loop
// (main.cc) or from the service timer — no concurrent access.
class ScreenSaverService {
public:
    // XBus message handler — returns true if message was consumed.
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    // Called on every user input event to reset the idle clock.
    // If the screensaver is currently active, also calls deactivate().
    static void resetIdleTimer() noexcept;

    // Called periodically (every 10 s from timer id=4) to check idle timeout.
    static void pollIdle() noexcept;

    // Deactivate without lock check — used when LockService is taking over.
    static void deactivate() noexcept;

    // A predicate consulted in pollIdle; while it returns true the idle clock is
    // held reset so the saver never fires (wired to WakeLock keep-awake).
    static void setWakeInhibitor(std::function<bool()> pred) noexcept;

    [[nodiscard]] static bool     isActive()      noexcept;
    [[nodiscard]] static uint32_t timeoutSeconds() noexcept;

private:
    static void activate() noexcept;
    static void broadcastState() noexcept;

    static std::atomic<bool>     active_;
    static std::function<bool()> wake_inhibitor_;
    static std::atomic<uint32_t> timeout_seconds_; // 0 = never
    static std::atomic<uint32_t> mode_;             // 0=blank, 1=clock, 2=matrix
    static std::atomic<uint32_t> inhibit_count_;
    static pid_t                 saver_pid_;
    static std::chrono::steady_clock::time_point last_input_;
};

} // namespace fbx::xserver
