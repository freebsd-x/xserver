#pragma once

#include "shared/protocol.h"
#include <memory>

namespace fbx::xserver {

class IpcSocket;

// com.freebsdx.LoginItems — "open at login" for each installed app. ListItems
// pages through the bundles (one per page); an app is on when its autostart
// entry ~/.config/autostart/freebsdx-<id>.desktop exists and is enabled.
// SetItem writes or removes that entry (xshell launches it at login by bundle
// id, so it gets its XAPP_ID) and broadcasts ItemsChanged — plus StartupItems'
// ItemsChanged, since the entry shows up there too.
class LoginItemsService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;
};

} // namespace fbx::xserver
