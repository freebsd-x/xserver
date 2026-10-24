#include "xserver/timezone_service.h"
#include "xserver/config_service.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <string>
#include <fstream>
#include <filesystem>
#include <mutex>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {

std::mutex g_mutex;
bool       g_applied = false;   // config value applied to TZ yet?

namespace fs = std::filesystem;

// The system's configured zone name (FreeBSD records it in /var/db/zoneinfo;
// otherwise derive it from the /etc/localtime symlink). "" if unknown.
std::string systemZone() {
    std::ifstream f("/var/db/zoneinfo");
    if (f) {
        std::string z; std::getline(f, z);
        while (!z.empty() && (z.back() == '\n' || z.back() == '\r' || z.back() == ' ')) z.pop_back();
        if (!z.empty()) return z;
    }
    std::error_code ec;
    auto tgt = fs::read_symlink("/etc/localtime", ec);
    if (!ec) {
        std::string s = tgt.string();
        if (auto p = s.find("zoneinfo/"); p != std::string::npos)
            return s.substr(p + 9);
    }
    return {};
}

bool zoneExists(const std::string& name) {
    if (name.empty() || name.find("..") != std::string::npos || name.front() == '/')
        return false;   // reject traversal / absolute paths
    std::error_code ec;
    return fs::exists("/usr/share/zoneinfo/" + name, ec);
}

// Apply the persisted zone (if any) to this process's TZ, once.
void ensureApplied() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_applied) return;
    g_applied = true;
    std::string z = ConfigService::get("timezone", "");
    if (!z.empty() && zoneExists(z)) {
        setenv("TZ", z.c_str(), 1);
        tzset();
    }
}

std::string currentZone() {
    std::string z = ConfigService::get("timezone", "");
    if (!z.empty()) return z;
    z = systemZone();
    return z.empty() ? std::string("UTC") : z;
}

shared::TimezoneState buildState() {
    shared::TimezoneState st{};
    std::string zone = currentZone();
    std::strncpy(st.tz_name_, zone.c_str(), sizeof(st.tz_name_) - 1);

    std::time_t now = std::time(nullptr);
    std::tm lt{};
    localtime_r(&now, &lt);
    st.utc_offset_sec_ = static_cast<int32_t>(lt.tm_gmtoff);
    std::strftime(st.local_time_, sizeof(st.local_time_), "%H:%M:%S", &lt);

    // display: "ABBR (UTC±H[:MM])"
    int off_min = st.utc_offset_sec_ / 60;
    char sign = off_min < 0 ? '-' : '+';
    int am = off_min < 0 ? -off_min : off_min;
    char disp[32];
    if (am % 60 == 0)
        std::snprintf(disp, sizeof(disp), "%s (UTC%c%d)", lt.tm_zone ? lt.tm_zone : "", sign, am / 60);
    else
        std::snprintf(disp, sizeof(disp), "%s (UTC%c%d:%02d)", lt.tm_zone ? lt.tm_zone : "",
                      sign, am / 60, am % 60);
    std::strncpy(st.display_name_, disp, sizeof(st.display_name_) - 1);
    return st;
}

void reply(std::shared_ptr<IpcSocket>& client, uint32_t tid, uint32_t opcode,
           const shared::TimezoneState& st) {
    if (!client) return;
    shared::XBusMessage m{};
    m.transaction_id_ = tid;
    std::strncpy(m.service_name_, "com.freebsdx.Timezone", sizeof(m.service_name_) - 1);
    m.opcode_ = opcode;
    std::memcpy(m.payload_, &st, sizeof(st));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

void broadcast(const shared::TimezoneState& st) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, "com.freebsdx.Timezone", sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::TimezoneOpcode::TZChanged);
    std::memcpy(b.payload_, &st, sizeof(st));
    BroadcastXBusMessage(b);
}

}  // namespace

shared::TimezoneState TimezoneService::currentState() noexcept {
    ensureApplied();
    return buildState();
}

void TimezoneService::handleMessage(const shared::XBusMessage& msg,
                                    std::shared_ptr<IpcSocket> client) noexcept {
    ensureApplied();
    using O = shared::TimezoneOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetTZ:
        reply(client, msg.transaction_id_, static_cast<uint32_t>(O::GetTZ), buildState());
        break;
    case O::SetTZ: {
        shared::TimezoneSetRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.tz_name_[sizeof(req.tz_name_) - 1] = '\0';
        std::string zone(req.tz_name_);
        if (zoneExists(zone)) {
            setenv("TZ", zone.c_str(), 1);
            tzset();
            ConfigService::set("timezone", zone);   // persist intent (session-applied)
            broadcast(buildState());
        }
        // Always reply so the caller isn't left waiting; reflects the (possibly
        // unchanged) current state.
        reply(client, msg.transaction_id_, static_cast<uint32_t>(O::SetTZ), buildState());
        break;
    }
    default:
        reply(client, msg.transaction_id_, msg.opcode_, buildState());
        break;
    }
}

}  // namespace fbx::xserver
