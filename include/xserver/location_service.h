#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// Backs both com.freebsdx.LocationServices (the master on/off toggle:
// GetState/SetEnabled/StateChanged) and com.freebsdx.Location (per-app
// precision: ListApps/SetAppPolicy/PolicyChanged) — they share LocationOpcode.
// Config-backed: the master toggle + per-app precision persist via ConfigService.
// There is no location provider wired, so requesting_count_ is 0 and the app
// list is empty until apps register (honest — no fabricated requesters).
class LocationService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
