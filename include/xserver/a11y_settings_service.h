#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
namespace fbx::xserver {
// com.freebsdx.A11ySettings — persisted quick accessibility prefs (font-size
// preset, cursor scale, high-contrast, magnifier) with per-field setters. Applying
// these visually needs toolkit/compositor hooks; this persists + broadcasts so the
// UI reflects them. (Wire name is "A11ySettings", not "AccessibilitySettings",
// because the latter overflows the 32-byte service_name_ field.)
class A11ySettingsService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};
}  // namespace fbx::xserver
