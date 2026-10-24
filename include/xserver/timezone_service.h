#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.Timezone — report and change the time zone.
//
// GetTZ replies with the current zone (name, abbreviation, UTC offset, local
// time). SetTZ applies a zone via TZ/tzset for this compositor process and
// persists the choice (a system-wide change to /etc/localtime needs root, which
// the unprivileged compositor lacks — so it degrades to session-scope + intent).
class TimezoneService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    // Current zone state (test/inspection seam).
    [[nodiscard]] static shared::TimezoneState currentState() noexcept;
};

}  // namespace fbx::xserver
