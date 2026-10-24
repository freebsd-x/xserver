#pragma once

#include "shared/protocol.h"
#include <memory>
#include <span>
#include <optional>

namespace fbx::xserver {
    class IpcSocket;
    class VulkanCompositor;
    class WindowStack;
}

namespace fbx::xserver {

class AutomationService {
public:
    [[nodiscard]] static std::optional<shared::InputMessage> ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg, VulkanCompositor* comp = nullptr, WindowStack* stack = nullptr) noexcept;
};

} // namespace fbx::xserver
