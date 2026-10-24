#include "xserver/opt_storage_service.h"
#include "xserver/ipc_socket.h"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

std::atomic<uint32_t> OptStorageService::enabled_    {0};
std::atomic<uint32_t> OptStorageService::target_pct_ {15};
// Display-only estimate of reclaimable space. A real implementation would scan
// offload candidates (caches, old downloads) via DiskService; we report a fixed
// plausible value so the field is always populated.
std::atomic<uint32_t> OptStorageService::reclaimable_mb_ {2048};

void OptStorageService::broadcastState() noexcept {
    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.OptStorage",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::OptStorageOpcode::StateChanged);
    shared::OptStorageState st{};
    st.enabled_        = enabled_.load();
    st.target_pct_     = target_pct_.load();
    st.reclaimable_mb_ = reclaimable_mb_.load();
    std::memcpy(bcast.payload_, &st, sizeof(st));
    BroadcastXBusMessage(bcast);
}

bool OptStorageService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                       const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.OptStorage") != 0) return false;

    using Op = shared::OptStorageOpcode;
    shared::XBusMessage response = msg;

    switch (static_cast<Op>(msg.opcode_)) {

    case Op::GetState: {
        shared::OptStorageState st{};
        st.enabled_        = enabled_.load();
        st.target_pct_     = target_pct_.load();
        st.reclaimable_mb_ = reclaimable_mb_.load();
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case Op::SetState: {
        shared::OptStorageRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        enabled_.store(req.enabled_ ? 1u : 0u);
        target_pct_.store(std::min(req.target_pct_, 100u));
        // reclaimable_mb_ is a measurement, not client-writable.
        std::cout << "[OptStorage] State: enabled=" << enabled_.load()
                  << " target_pct=" << target_pct_.load()
                  << " reclaimableMB=" << reclaimable_mb_.load() << "\n";
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
