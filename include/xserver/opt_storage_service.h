#pragma once
#include "shared/protocol.h"
#include <memory>
#include <atomic>

namespace fbx::xserver {
class IpcSocket;

// com.freebsdx.OptStorage — auto-offload rarely-used files when disk gets tight.
// Holds the authoritative enabled/target-free-space state plus a display-only
// estimate of how much could be reclaimed right now (SetState never writes it).
class OptStorageService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    static bool     isEnabled()      noexcept { return enabled_.load() != 0; }
    static uint32_t targetPct()      noexcept { return target_pct_.load(); }
    static uint32_t reclaimableMb()  noexcept { return reclaimable_mb_.load(); }

private:
    static std::atomic<uint32_t> enabled_;
    static std::atomic<uint32_t> target_pct_;
    static std::atomic<uint32_t> reclaimable_mb_;  // display-only estimate

    static void broadcastState() noexcept;
};

} // namespace fbx::xserver
