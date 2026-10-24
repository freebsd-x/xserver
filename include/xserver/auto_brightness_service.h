#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
namespace fbx::xserver {
// com.freebsdx.AutoBrightness — persisted auto-brightness toggle. No ambient-light
// sensor is available, so lux is reported as UINT32_MAX (sensor unavailable).
class AutoBrightnessService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};
}  // namespace fbx::xserver
