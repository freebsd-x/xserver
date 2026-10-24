#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.NetConns — read-only active-socket list (NetConnOpcode::ListConns
// → paged ConnPage). Enumerates via sockstat(1); see net_conn.h for the parser.
class NetConnService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
