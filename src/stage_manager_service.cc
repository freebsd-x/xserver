#include "xserver/stage_manager_service.h"
#include "xserver/ipc_socket.h"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

std::atomic<uint32_t> StageManagerService::enabled_      {0};
std::atomic<uint32_t> StageManagerService::recent_count_ {4};  // 1-6

void StageManagerService::broadcastState() noexcept {
    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.StageManager",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::StageManagerOpcode::StateChanged);
    shared::StageManagerState st{};
    st.enabled_      = enabled_.load();
    st.recent_count_ = recent_count_.load();
    std::memcpy(bcast.payload_, &st, sizeof(st));
    BroadcastXBusMessage(bcast);
}

bool StageManagerService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                         const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.StageManager") != 0) return false;

    using Op = shared::StageManagerOpcode;
    shared::XBusMessage response = msg;

    switch (static_cast<Op>(msg.opcode_)) {

    case Op::GetState: {
        shared::StageManagerState st{};
        st.enabled_      = enabled_.load();
        st.recent_count_ = recent_count_.load();
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case Op::SetState: {
        shared::StageManagerRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        enabled_.store(req.enabled_ ? 1u : 0u);
        recent_count_.store(std::clamp(req.recent_count_, 1u, 6u));
        std::cout << "[StageManager] State: enabled=" << enabled_.load()
                  << " recent_count=" << recent_count_.load() << "\n";
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        broadcastState();
        break;
    }

    default:
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    return true;
}

} // namespace fbx::xserver
