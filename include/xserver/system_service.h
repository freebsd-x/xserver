#pragma once

#include "shared/protocol.h"
#include <memory>

namespace fbx::xserver {

class IpcSocket;

class SystemService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept;

    static void incrementFrameCount() noexcept { frame_count_++; }

private:
    static shared::SystemInfo getSystemStats() noexcept;
    static uint32_t frame_count_;
};

} // namespace fbx::xserver
