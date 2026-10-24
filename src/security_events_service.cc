#include "xserver/security_events_service.h"
#include "shared/logger.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

std::mutex                        SecurityEventsService::mutex_;
std::deque<shared::SecurityEvent> SecurityEventsService::events_;
std::string                       SecurityEventsService::log_path_;

namespace {
void setField(char* dst, size_t cap, const std::string& s) {
    std::memset(dst, 0, cap);
    std::strncpy(dst, s.c_str(), cap - 1);
}
}  // namespace

void SecurityEventsService::init() noexcept {
    const char* home = std::getenv("HOME");
    std::string base = home ? std::string(home) + "/.freebsdx" : "/tmp/.freebsdx";
    std::error_code ec;
    fs::create_directories(base, ec);
    log_path_ = base + "/security-events";
    loadFromDisk();
    LOG_INFO("SecurityEventsService: ready ({} events)", events_.size());
}

// Persistence: one event per line — type|timestamp|user|source|success.
// user/source are sanitized on the way in (no control chars), so a plain
// pipe-split round-trips safely.
bool SecurityEventsService::loadFromDisk() noexcept {
    std::ifstream f(log_path_);
    if (!f.is_open()) return true;
    events_.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::stringstream ss(line);
        std::string type, ts, user, source, success;
        std::getline(ss, type,    '|');
        std::getline(ss, ts,      '|');
        std::getline(ss, user,    '|');
        std::getline(ss, source,  '|');
        std::getline(ss, success, '|');
        if (type.empty()) continue;
        shared::SecurityEvent e{};
        setField(e.type_,   sizeof(e.type_),   type);
        setField(e.user_,   sizeof(e.user_),   user);
        setField(e.source_, sizeof(e.source_), source);
        e.timestamp_ = ts.empty()      ? 0u : static_cast<uint64_t>(std::stoull(ts));
        e.success_   = success == "1" ? 1u : 0u;
        events_.push_back(e);
        if (events_.size() > kMaxEvents) events_.pop_back();
    }
    return true;
}

bool SecurityEventsService::saveToDisk() noexcept {
    std::ofstream f(log_path_, std::ios::trunc);
    if (!f.is_open()) return false;
    f << "# FreeBSD-X Security Events v1\n";
    for (const auto& e : events_)
        f << e.type_ << '|' << e.timestamp_ << '|' << e.user_ << '|'
          << e.source_ << '|' << (e.success_ ? 1 : 0) << '\n';
    return true;
}

void SecurityEventsService::record(const std::string& type, const std::string& user,
                                   const std::string& source, bool success) noexcept {
    shared::SecurityEvent e{};
    setField(e.type_,   sizeof(e.type_),   type);
    // Strip anything that would break the pipe-delimited log or inject control
    // characters into the audit trail (usernames come from untrusted input).
    auto clean = [](std::string s) {
        for (char& c : s) if (c == '|' || c == '\n' || c == '\r' || c < 0x20) c = '?';
        return s;
    };
    setField(e.user_,   sizeof(e.user_),   clean(user));
    setField(e.source_, sizeof(e.source_), clean(source.empty() ? "local" : source));
    e.timestamp_ = static_cast<uint64_t>(std::time(nullptr));
    e.success_   = success ? 1u : 0u;

    {
        std::lock_guard<std::mutex> lk(mutex_);
        events_.push_front(e);
        while (events_.size() > kMaxEvents) events_.pop_back();
        saveToDisk();
    }

    // Broadcast the new entry (transaction_id_ 0 = unsolicited).
    shared::XBusMessage bcast{};
    std::strncpy(bcast.service_name_, "com.freebsdx.Security",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::SecurityOpcode::EventAdded);
    std::memcpy(bcast.payload_, &e, sizeof(e));
    BroadcastXBusMessage(bcast);
}

void SecurityEventsService::sendPage(IpcSocket* client, uint32_t tid, uint32_t page) noexcept {
    shared::SecurityEventPage pg{};
    pg.page_  = page;
    pg.total_ = static_cast<uint32_t>(events_.size());
    const uint32_t start = page * 3;
    uint32_t n = 0;
    for (uint32_t i = start; i < events_.size() && n < 3; ++i, ++n)
        pg.entries_[n] = events_[i];
    pg.count_    = n;
    pg.has_more_ = (start + n < events_.size()) ? 1u : 0u;

    shared::XBusMessage reply{};
    reply.transaction_id_ = tid;   // echo so the client's Call callback matches
    std::strncpy(reply.service_name_, "com.freebsdx.Security",
                 sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(shared::SecurityOpcode::ListEvents);
    std::memcpy(reply.payload_, &pg, sizeof(pg));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

void SecurityEventsService::handleMessage(const shared::XBusMessage& msg,
                                          std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::SecurityOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListEvents: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));
        std::lock_guard<std::mutex> lk(mutex_);
        sendPage(client.get(), msg.transaction_id_, page);
        break;
    }
    case O::ClearEvents: {
        std::lock_guard<std::mutex> lk(mutex_);
        events_.clear();
        saveToDisk();
        LOG_INFO("SecurityEventsService: cleared");
        break;
    }
    default:
        break;
    }
}

}  // namespace fbx::xserver
