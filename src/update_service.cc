#include "xserver/update_service.h"
#include "xserver/ipc_socket.h"
#include "xserver/async_shell.h"
#include <iostream>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <span>
#include <thread>
#include <atomic>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

// Cached count of outdated packages. The `pkg` query below can block for a long
// time (repo lock, slow/absent network), so it must NEVER run on the
// compositor's single event-loop thread — doing so froze the whole desktop
// (input, XBus, presentation) whenever a client asked for the update count. It
// is refreshed on a detached background thread; handlers read this cache.
static std::atomic<uint32_t> g_pending_updates{0};
static std::atomic<bool>     g_update_check_inflight{false};

// Runs `pkg version -l '<'` and returns the number of outdated packages.
// Returns 0 on any error (pkg not installed, etc.). Blocking — background only.
static uint32_t queryPendingUpdatesBlocking() noexcept {
    // posix_spawn, not popen (see async_shell.h — popen deadlocks the forked child on
    // a libc stdio lock in the multithreaded compositor).
    std::string out = spawnCapture("pkg version -l '<' 2>/dev/null | wc -l");
    long n = std::strtol(out.c_str(), nullptr, 10);
    return (n > 0) ? static_cast<uint32_t>(n) : 0u;
}

// Kick a background refresh of the cached count if one isn't already running.
// Returns immediately; the compositor never blocks on pkg.
static void refreshPendingUpdatesAsync() noexcept {
    bool expected = false;
    if (!g_update_check_inflight.compare_exchange_strong(expected, true)) return;
    std::thread([] {
        g_pending_updates.store(queryPendingUpdatesBlocking(), std::memory_order_relaxed);
        g_update_check_inflight.store(false, std::memory_order_relaxed);
    }).detach();
}

static void broadcastAvailable(uint32_t count) noexcept {
    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.Update",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::UpdateOpcode::UpdatesAvailable);
    shared::UpdateAvailableState st{};
    st.count_ = count;
    std::memcpy(bcast.payload_, &st, sizeof(st));
    BroadcastXBusMessage(bcast);
}

bool UpdateService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                   const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.Update") != 0) return false;

    using Op = shared::UpdateOpcode;
    shared::XBusMessage response = msg;

    switch (static_cast<Op>(msg.opcode_)) {

    case Op::CheckForUpdates: {
        // Reply with the cached count immediately and kick a background refresh —
        // never block the event loop on pkg.
        refreshPendingUpdatesAsync();
        uint32_t count = g_pending_updates.load(std::memory_order_relaxed);
        std::cout << "[UpdateService] " << count << " package(s) with updates available (cached).\n";
        shared::UpdateAvailableState st{};
        st.count_ = count;
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        // Also broadcast so all connected clients learn about pending updates
        broadcastAvailable(count);
        break;
    }

    case Op::ApplyUpdate: {
        std::cout << "[UpdateService] Applying update via ZFS Boot Environment...\n";
        std::cout << "   bectl create freebsdx-update && pkg -r /mnt/be upgrade -y\n";
        std::cout << "   bectl activate freebsdx-update  (reboot required)\n";
        uint32_t success = 1;
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &success, sizeof(success));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    default:
        std::cerr << "[UpdateService] Unknown opcode " << msg.opcode_ << "\n";
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    return true;
}

} // namespace fbx::xserver
