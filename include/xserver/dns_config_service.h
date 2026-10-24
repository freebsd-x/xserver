#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.DnsConfig — resolver settings (primary/secondary/DoH) + cache
// flush. Distinct from com.freebsdx.DNS (mode toggle). GetConfig reports the
// persisted config (defaulting primary to the current resolv.conf nameserver);
// SetConfig validates + persists + best-effort writes /etc/resolv.conf (root →
// logged if denied) + acks + broadcasts ConfigChanged; Flush best-effort
// reloads the resolver cache (root → logged) + acks.
class DnsConfigService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
