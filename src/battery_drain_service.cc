#include "xserver/battery_drain_service.h"
#include "xserver/battery_drain.h"
#include "xserver/proc_enum.h"
#include "shared/logger.h"

#include <cstring>
#include <vector>

namespace fbx::xserver {

void BatteryDrainService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::BatteryDrainOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListDrain: {
        uint32_t page = 0; std::memcpy(&page, msg.payload_, sizeof(page));
        auto drain = batdrain::computeDrain(proc::enumerateProcesses());
        auto pg = batdrain::buildPage(drain, page);

        shared::XBusMessage reply{};
        reply.transaction_id_ = msg.transaction_id_;
        std::strncpy(reply.service_name_, "com.freebsdx.BatteryDrain", sizeof(reply.service_name_) - 1);
        reply.opcode_ = static_cast<uint32_t>(O::ListDrain);
        std::memcpy(reply.payload_, &pg, sizeof(pg));
        if (client) client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
        break;
    }
    case O::RefreshDrain: {
        // Each ListDrain re-samples live, so there's nothing to refresh — ack.
        shared::XBusMessage ack{};
        ack.transaction_id_ = msg.transaction_id_;
        std::strncpy(ack.service_name_, "com.freebsdx.BatteryDrain", sizeof(ack.service_name_) - 1);
        ack.opcode_ = static_cast<uint32_t>(O::RefreshDrain);
        if (client) client->sendMessage({reinterpret_cast<const uint8_t*>(&ack), sizeof(ack)});
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
