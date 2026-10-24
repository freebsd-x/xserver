#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.Keymap — active XKB layout for seat0. SetLayout persists the
// chosen layout/variant and broadcasts LayoutChanged (the shell's confirmation);
// there is no direct reply per the contract. The real xkbcommon remap in the
// input path is a follow-up — the compositor currently does its own scancode
// handling (KDSKBMODE K_CODE), so this persists + confirms the selection.
class KeymapService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
