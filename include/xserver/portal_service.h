#pragma once

#include "shared/protocol.h"
#include <memory>
#include <string>

namespace fbx::xserver {

class IpcSocket;

class PortalService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept;

private:
    static void handleOpenFile(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept;
    static void handleListDir(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept;
    static void handleZfsRollback(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept;
    static void handleZfsSnapshot(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept;
};

} // namespace fbx::xserver
