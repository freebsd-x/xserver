#pragma once

#include <memory>
#include <mutex>
#include <shared/protocol.h>
#include "xserver/ipc_socket.h"

namespace fbx {

using xserver::IpcSocket;

class MediaPlayerService {
public:
    static void init() noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
    // Called from the main input event loop when XF86 media keys are detected.
    // key: 0=play/pause, 1=stop, 2=next, 3=prev
    static void broadcastMediaKey(uint32_t key) noexcept;

private:
    static std::mutex              mutex_;
    static shared::MediaPlayerState last_state_;
    static bool                    has_state_;
};

} // namespace fbx
