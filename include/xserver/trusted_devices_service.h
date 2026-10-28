#pragma once

#include "shared/protocol.h"
#include <memory>

namespace fbx::xserver {

class IpcSocket;

// com.freebsdx.TrustedDevices — the Bluetooth devices this system knows
// (/etc/bluetooth/hosts: "bdaddr name", world-readable). Whether one is
// trusted (paired, no prompt) lives in hcsecd.conf, which only root can read
// or write, so trusted_ is reported 0 and SetTrust is refused and logged.
// No Bluetooth devices configured → an empty list.
class TrustedDevicesService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;
};

} // namespace fbx::xserver
