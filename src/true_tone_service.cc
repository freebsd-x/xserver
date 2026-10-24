#include "xserver/true_tone_service.h"
#include "xserver/ipc_socket.h"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

std::atomic<uint32_t> TrueToneService::enabled_   {0};
std::atomic<uint32_t> TrueToneService::strength_  {70};
// Measured ambient color temperature. A real implementation would read an
// ambient-light/color sensor; we report a fixed warm-indoor value (5000 K) so
// the display-only field is always populated.
std::atomic<uint32_t> TrueToneService::ambient_k_ {5000};

void TrueToneService::broadcastState() noexcept {
    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.TrueTone",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::TrueToneOpcode::StateChanged);
    shared::TrueToneState st{};
    st.enabled_   = enabled_.load();
    st.strength_  = strength_.load();
    st.ambient_k_ = ambient_k_.load();
    std::memcpy(bcast.payload_, &st, sizeof(st));
    BroadcastXBusMessage(bcast);
}

bool TrueToneService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                     const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.TrueTone") != 0) return false;

    using Op = shared::TrueToneOpcode;
    shared::XBusMessage response = msg;

    switch (static_cast<Op>(msg.opcode_)) {

    case Op::GetState: {
        shared::TrueToneState st{};
        st.enabled_   = enabled_.load();
        st.strength_  = strength_.load();
        st.ambient_k_ = ambient_k_.load();
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case Op::SetState: {
        shared::TrueToneRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        enabled_.store(req.enabled_ ? 1u : 0u);
        strength_.store(std::min(req.strength_, 100u));
        // ambient_k_ is measured, not client-writable.
        std::cout << "[TrueTone] State: enabled=" << enabled_.load()
                  << " strength=" << strength_.load()
                  << " ambientK=" << ambient_k_.load() << "\n";
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
