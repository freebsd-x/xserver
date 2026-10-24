#include "xserver/smart_service.h"
#include "xserver/smart_list.h"
#include "xserver/async_shell.h"
#include "shared/logger.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fbx::xserver {

namespace {

// Enumerate disks (sysctl kern.disks — always readable) then query SMART per
// drive via smartctl (needs the port + root; absent/denied → health stays N/A).
// posix_spawn, not popen (popen deadlocks the threaded compositor — async_shell).
std::vector<smart::SmartInfo> enumerate() {
    std::vector<smart::SmartInfo> drives;
    for (auto& name : smart::parseDiskList(spawnCapture("sysctl -n kern.disks 2>/dev/null"))) {
        smart::SmartInfo d;
        d.name = name;
        char cmd[96];
        std::snprintf(cmd, sizeof(cmd), "smartctl -H -A /dev/%s 2>/dev/null", name.c_str());
        smart::parseSmartctl(spawnCapture(cmd), d);
        drives.push_back(std::move(d));
    }
    return drives;
}

}  // namespace

void SmartService::handleMessage(const shared::XBusMessage& msg,
                                 std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::SmartOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListDrives: {
        uint32_t page = 0; std::memcpy(&page, msg.payload_, sizeof(page));
        auto drives = enumerate();

        shared::SmartPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(drives.size());
        uint32_t start = page * 3, n = 0;
        for (; n < 3 && start + n < drives.size(); ++n) {
            const smart::SmartInfo& d = drives[start + n];
            shared::SmartDrive& e = pg.entries_[n];
            std::strncpy(e.name_,   d.name.c_str(),   sizeof(e.name_) - 1);
            std::strncpy(e.health_, d.health.c_str(), sizeof(e.health_) - 1);
            e.temp_c_         = d.temp_c;
            e.reallocated_    = d.reallocated;
            e.power_on_hours_ = d.power_on_hours;
        }
        pg.count_    = n;
        pg.has_more_ = (start + n < drives.size()) ? 1u : 0u;

        shared::XBusMessage reply{};
        reply.transaction_id_ = msg.transaction_id_;
        std::strncpy(reply.service_name_, "com.freebsdx.Smart", sizeof(reply.service_name_) - 1);
        reply.opcode_ = static_cast<uint32_t>(O::ListDrives);
        std::memcpy(reply.payload_, &pg, sizeof(pg));
        client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
