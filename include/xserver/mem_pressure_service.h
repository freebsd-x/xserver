#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.MemPressure — physical-memory usage and a derived pressure level,
// read live from FreeBSD vm sysctls.
class MemPressureService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    [[nodiscard]] static shared::MemPressureState readState() noexcept;
};

}  // namespace fbx::xserver
