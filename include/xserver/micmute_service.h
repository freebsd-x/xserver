#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <functional>
#include <memory>

namespace fbx::xserver {

// com.freebsdx.MicMute — mic mute. Persisted + broadcast, and actually gated at
// the OSS mixer via the apply hook registered in init().
class MicMuteService {
public:
    // apply(muted) is called on change + once at startup with the persisted
    // value (wired in main.cc to applyToMixer). Left unset in tests so the
    // handler has no hardware side effects.
    static void init(std::function<void(bool)> apply) noexcept;
    // Zero the mic input level at /dev/mixer when muted; restore it when not.
    static void applyToMixer(bool muted) noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
