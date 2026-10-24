#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.Proxy — session HTTP/HTTPS proxy config.
//   GetConfig → ProxyConfig    SetConfig(cfg) → persist + ConfigChanged
//   broadcast. Persists to ~/.freebsdx/proxy and mirrors http_proxy/https_proxy
//   into the compositor env so apps it launches inherit the setting.
class ProxyService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
