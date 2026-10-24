#pragma once
#include "shared/protocol.h"
#include <memory>
#include <atomic>

namespace fbx::xserver {
class IpcSocket;

// com.freebsdx.MonoAudio — accessibility: collapse stereo to mono and shift the
// left/right balance. Holds the authoritative enabled/balance state that the
// mixer routing consults; shell drives it via the Mono Audio quick setting.
class MonoAudioService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    static bool     isEnabled() noexcept { return enabled_.load() != 0; }
    static uint32_t balance()   noexcept { return balance_.load(); }

private:
    static std::atomic<uint32_t> enabled_;
    static std::atomic<uint32_t> balance_;

    static void broadcastState() noexcept;
};

} // namespace fbx::xserver
