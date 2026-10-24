#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.KernelModules — loaded kernel modules (KmodOpcode::ListModules →
// paged KmodPage, 3/page) parsed from kldstat(8) (base-system, unprivileged
// read). UnloadModule needs root (kldunload) → logged for now. flags_ is 0
// (kldstat doesn't expose kld flags).
class KmodService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
