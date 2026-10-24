#include "xserver/process_service.h"
#include "xserver/proc_list.h"
#include "xserver/proc_enum.h"
#include "shared/logger.h"

#include <csignal>
#include <cstring>
#include <vector>

namespace fbx::xserver {

void ProcessService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::ProcessOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListTop: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));
        auto procs = proc::enumerateProcesses();
        proc::sortByCpuDesc(procs);
        shared::ProcessPage pg = proc::buildPage(procs, page);

        shared::XBusMessage reply{};
        reply.transaction_id_ = msg.transaction_id_;
        std::strncpy(reply.service_name_, "com.freebsdx.Process",
                     sizeof(reply.service_name_) - 1);
        reply.opcode_ = static_cast<uint32_t>(O::ListTop);
        std::memcpy(reply.payload_, &pg, sizeof(pg));
        client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
        break;
    }
    case O::Kill: {
        shared::ProcessKillRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        int sig = (req.signal_ == 9) ? SIGKILL : SIGTERM;
        if (req.pid_ > 1) {
            if (::kill(static_cast<pid_t>(req.pid_), sig) == 0)
                LOG_INFO("ProcessService: signalled pid {} ({})", req.pid_, sig);
            else
                LOG_WARN("ProcessService: cannot signal pid {} (perm/gone)", req.pid_);
        }
        shared::XBusMessage ack{};
        ack.transaction_id_ = msg.transaction_id_;
        std::strncpy(ack.service_name_, "com.freebsdx.Process",
                     sizeof(ack.service_name_) - 1);
        ack.opcode_ = static_cast<uint32_t>(O::Kill);
        client->sendMessage({reinterpret_cast<const uint8_t*>(&ack), sizeof(ack)});
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
