#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.AudioOutput — enumerate/switch audio output sinks. ListSinks
// reads /dev/sndstat (unprivileged) and marks the "default" device active;
// SetActiveSink writes hw.snd.default_unit (needs root → best-effort, logged)
// then broadcasts the refreshed list.
class AudioOutputService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
