#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <string>
#include <functional>

namespace fbx::xserver {

// com.freebsdx.Keymap — the keyboard layout. SetLayout validates, applies it
// to the input path (InputManager::setKeymap via the hook init() installs —
// characters then come from that XKB layout), persists it and broadcasts
// LayoutChanged (the shell's confirmation; there's no direct reply). A layout
// xkbcommon can't build is refused and not saved. init() applies the saved
// layout (default "us").
class KeymapService {
public:
    static void init(std::function<bool(const std::string& layout, const std::string& variant)> apply) noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
