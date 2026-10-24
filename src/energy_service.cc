#include "xserver/energy_service.h"
#include "xserver/energy.h"
#include "xserver/proc_enum.h"

#include <cstring>
#include <vector>

namespace fbx::xserver {

void EnergyService::handleMessage(const shared::XBusMessage& msg,
                                  std::shared_ptr<IpcSocket> client) noexcept {
    if (static_cast<shared::EnergyOpcode>(msg.opcode_) != shared::EnergyOpcode::ListUsage)
        return;

    uint32_t page = 0; std::memcpy(&page, msg.payload_, sizeof(page));
    auto usage = energy::computeUsage(proc::enumerateProcesses());
    auto pg = energy::buildPage(usage, page);

    shared::XBusMessage reply{};
    reply.transaction_id_ = msg.transaction_id_;
    std::strncpy(reply.service_name_, "com.freebsdx.Energy", sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(shared::EnergyOpcode::ListUsage);
    std::memcpy(reply.payload_, &pg, sizeof(pg));
    if (client) client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

}  // namespace fbx::xserver
