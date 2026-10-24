#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.DNS — resolver mode (Auto/DoH/Custom).
//   GetConfig → DnsConfig    SetMode(req) → persist + best-effort resolv.conf +
//   ConfigChanged broadcast. The chosen mode persists to ~/.freebsdx/dns; the
//   Auto label reflects the live /etc/resolv.conf nameserver.
class DnsService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
