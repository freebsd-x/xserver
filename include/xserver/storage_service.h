#pragma once

#include "shared/protocol.h"
#include <memory>

namespace fbx::xserver {
    class IpcSocket;
}

namespace fbx::xserver {

class StorageService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept;
};

} // namespace fbx::xserver
