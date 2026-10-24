#pragma once

#include "shared/protocol.h"
#include <memory>

namespace fbx::xserver {

class IpcSocket;

class HardwareService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept;
    
    static shared::HardwareStatus getStatus() noexcept;
};

} // namespace fbx::xserver
