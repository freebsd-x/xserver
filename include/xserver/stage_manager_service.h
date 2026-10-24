#pragma once
#include "shared/protocol.h"
#include <memory>
#include <atomic>

namespace fbx::xserver {
class IpcSocket;

// com.freebsdx.StageManager — group windows into a focused stage with a strip of
// recent apps. Holds the authoritative enabled/recent-strip-size state that the
// shell drives via the Stage Manager quick setting.
class StageManagerService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    static bool     isEnabled()    noexcept { return enabled_.load() != 0; }
    static uint32_t recentCount()  noexcept { return recent_count_.load(); }

private:
    static std::atomic<uint32_t> enabled_;
    static std::atomic<uint32_t> recent_count_;

    static void broadcastState() noexcept;
};

} // namespace fbx::xserver
