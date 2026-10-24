#include "xserver/mono_audio_service.h"
#include "xserver/ipc_socket.h"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

std::atomic<uint32_t> MonoAudioService::enabled_ {0};
std::atomic<uint32_t> MonoAudioService::balance_ {50};  // 50 = centered

void MonoAudioService::broadcastState() noexcept {
    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.MonoAudio",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::MonoAudioOpcode::StateChanged);
    shared::MonoAudioState st{};
    st.enabled_ = enabled_.load();
    st.balance_ = balance_.load();
    std::memcpy(bcast.payload_, &st, sizeof(st));
    BroadcastXBusMessage(bcast);
}

bool MonoAudioService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                      const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.MonoAudio") != 0) return false;

    using Op = shared::MonoAudioOpcode;
    shared::XBusMessage response = msg;

    switch (static_cast<Op>(msg.opcode_)) {

    case Op::GetState: {
        shared::MonoAudioState st{};
        st.enabled_ = enabled_.load();
        st.balance_ = balance_.load();
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case Op::SetState: {
        shared::MonoAudioRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        enabled_.store(req.enabled_ ? 1u : 0u);
        balance_.store(std::min(req.balance_, 100u));
        std::cout << "[MonoAudio] State: enabled=" << enabled_.load()
                  << " balance=" << balance_.load() << "\n";
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
