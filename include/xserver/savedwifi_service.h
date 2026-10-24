#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.SavedWifi — saved wireless networks from wpa_supplicant.conf.
//   ListSaved(page) → SavedWifiPage (one network per page)
//   SetAutoConnect / ForgetNetwork → ack (+ SavedListChanged broadcast on forget)
// Reads /etc/wpa_supplicant.conf (root-only in practice → empty list when
// unreadable). Mutations need root/wpa_cli; persisted-intent + logged for now.
class SavedWifiService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
