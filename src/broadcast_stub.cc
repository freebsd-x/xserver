// broadcast_stub.cc — no-op BroadcastXBusMessage for standalone service daemons.
// The full implementation (xbus_dispatcher.cc) requires the Vulkan compositor
// and is only linked into the freebsdx compositor executable.
#include "shared/protocol.h"

namespace fbx::xserver {

void BroadcastXBusMessage(const fbx::shared::XBusMessage& /*msg*/) noexcept {
    // Standalone service daemons do not maintain a client socket list.
    // Broadcasts are delivered by the compositor process, not the daemon.
}

} // namespace fbx::xserver
