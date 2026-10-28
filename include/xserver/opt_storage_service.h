#pragma once

#include "shared/protocol.h"
#include <atomic>
#include <cstdint>
#include <ctime>
#include <memory>

namespace fbx::xserver {

class IpcSocket;

// com.freebsdx.OptStorage — free space by clearing old cache files. What it
// may delete: regular files under ~/.cache (XDG: safe to remove) not modified
// or read for 30 days; symlinks are never followed or removed.
// reclaimable_mb_ is that total, measured on a worker thread (StateChanged is
// broadcast when a scan finishes). When enabled and the home filesystem's free
// space drops below target_pct_, poll() (≤ once an hour) clears them.
// enabled/target persist in ConfigService "optstorage.*".
class OptStorageService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;
    static void poll() noexcept;   // from a compositor timer

    // A cache file is reclaimable when untouched for 30 days.
    static constexpr std::time_t kMaxAge = 30 * 24 * 3600;
    [[nodiscard]] static bool isStale(std::time_t mtime, std::time_t atime, std::time_t now) noexcept {
        return now - mtime > kMaxAge && now - atime > kMaxAge;
    }

private:
    static void broadcastState() noexcept;
    static void startScan(bool clean) noexcept;
    static std::atomic<uint32_t> reclaimable_mb_;
    static std::atomic<bool>     busy_;
    static std::atomic<bool>     clean_requested_;   // survives a scan already running
};

} // namespace fbx::xserver
