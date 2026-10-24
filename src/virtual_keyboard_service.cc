#include "xserver/virtual_keyboard_service.h"
#include "shared/protocol.h"

namespace fbx::xserver {

std::mutex                            VirtualKeyboardService::mutex_;
std::vector<shared::VirtualKeyInput>  VirtualKeyboardService::queue_;

void VirtualKeyboardService::queueKey(const shared::VirtualKeyInput& key) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    queue_.push_back(key);
}

std::vector<shared::InputMessage> VirtualKeyboardService::drainInjectedKeys() noexcept {
    std::vector<shared::VirtualKeyInput> pending;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pending.swap(queue_);
    }

    std::vector<shared::InputMessage> result;
    result.reserve(pending.size());

    for (const auto& vk : pending) {
        // Key Press — xkit apps use stateless key handling; one KeyPress is sufficient.
        shared::InputMessage press{};
        press.type_      = shared::InputEventType::KeyPress;
        press.value_a_   = static_cast<int32_t>(vk.keycode_);
        press.modifiers_ = vk.modifiers_;
        result.push_back(press);
    }
    return result;
}

} // namespace fbx::xserver
