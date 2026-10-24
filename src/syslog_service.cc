#include "xserver/syslog_service.h"
#include "xserver/syslog_parse.h"
#include "shared/logger.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace fbx::xserver {

namespace {
// Read up to the last kMax lines of /var/log/messages, newest first.
std::vector<std::string> tailMessages() {
    constexpr size_t kMax = 200;
    std::vector<std::string> lines;
    std::ifstream f("/var/log/messages");
    if (!f.is_open()) {
        LOG_WARN("SysLogService: /var/log/messages unreadable (needs privilege)");
        return lines;
    }
    std::string line;
    while (std::getline(f, line)) {
        lines.push_back(line);
        if (lines.size() > kMax) lines.erase(lines.begin());   // keep the tail
    }
    std::reverse(lines.begin(), lines.end());                  // newest first
    return lines;
}
}  // namespace

void SysLogService::handleMessage(const shared::XBusMessage& msg,
                                  std::shared_ptr<IpcSocket> client) noexcept {
    if (static_cast<shared::SysLogOpcode>(msg.opcode_) != shared::SysLogOpcode::GetEntries)
        return;

    shared::SysLogRequest req{};
    std::memcpy(&req, msg.payload_, sizeof(req));

    std::vector<std::string> lines = tailMessages();

    shared::SysLogPageFlat pg{};
    pg.page_  = req.page_;
    pg.total_ = static_cast<uint32_t>(lines.size());
    if (req.page_ < lines.size()) {
        syslog::Entry e = syslog::parseLine(lines[req.page_]);
        pg.valid_ = 1;
        pg.level_ = e.level;
        std::strncpy(pg.facility_, e.facility.c_str(), sizeof(pg.facility_) - 1);
        std::strncpy(pg.message_,  e.message.c_str(),  sizeof(pg.message_) - 1);
    }
    pg.has_more_ = (req.page_ + 1 < lines.size()) ? 1u : 0u;

    shared::XBusMessage reply{};
    reply.transaction_id_ = msg.transaction_id_;
    std::strncpy(reply.service_name_, "com.freebsdx.SysLog",
                 sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(shared::SysLogOpcode::GetEntries);
    std::memcpy(reply.payload_, &pg, sizeof(pg));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

}  // namespace fbx::xserver
