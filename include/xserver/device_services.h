#pragma once

#include "shared/protocol.h"
#include "xserver/input_devices.h"
#include "xserver/ipc_socket.h"
#include <functional>
#include <memory>
#include <vector>

namespace fbx::xserver {

// com.freebsdx.PrintQueue — active CUPS jobs via libcups (cupsGetJobs2), 2 per
// page. CancelJob → cupsCancelJob2 as the session user, so CUPS only lets the
// user cancel their own jobs. With cupsd down the list is simply empty.
class PrintQueueService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.InputDevice — live libinput devices (one per page) and a
// per-device pointer speed that overrides com.freebsdx.Pointer for that device.
// Overrides persist (ConfigService "input.device_overrides") and survive
// replugs; init() wires the InputManager hooks and re-applies them.
class InputDeviceService {
public:
    using Lister = std::function<std::vector<inputdev::DeviceInfo>()>;
    using Setter = std::function<void(uint32_t id, uint32_t sensitivity, bool adaptive)>;
    static void init(Lister list, Setter set) noexcept;
    // Hot-plug: tell the shell to re-list (wired to InputManager in main).
    static void notifyDevicesChanged() noexcept;

    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.SecurityKey — the FIDO2/U2F keys plugged in now (USB devices
// from devinfo, recognised by vendor), labelled by their device description.
// Registering a key for login needs pam_u2f (not installed), so RegisterKey
// and RemoveKey are refused with a logged reason.
class SecurityKeyService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
