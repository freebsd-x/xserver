#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// Three USB-enumeration services, all backed by usbconfig(8) via one shared
// parser (usb_list.h):
//   • com.freebsdx.Usb         — UsbOpcode,      one UsbDevice per page + Eject
//   • com.freebsdx.UsbDevices  — UsbDevOpcode,   three UsbDevEntry per page + Rescan
//   • com.freebsdx.UsbDevice   — UsbDeviceOpcode, one detail page per device + Eject
// All read-only enumeration (Eject is logged — needs root/umount). Degrade to an
// empty list when usbconfig is unavailable.
class UsbService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

class UsbDevicesService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

class UsbDeviceService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
