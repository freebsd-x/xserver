#include "xserver/sysadmin_services.h"
#include "xserver/sysadmin_parse.h"
#include "xserver/xbus_reply.h"
#include "xserver/async_shell.h"
#include "xserver/priv_broker.h"
#include "xserver/priv_ops.h"
#include "xserver/proc_enum.h"
#include "shared/logger.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fbx::xserver {

namespace {

using namespace svc;

using Clock = std::chrono::steady_clock;

// The shell walks paged lists one request per page (3 rows each), so a single
// refresh of 41 boot environments would run bectl 14 times. Cache each
// command's output briefly; mutations invalidate.
std::mutex g_cache_mu;
std::map<std::string, std::pair<Clock::time_point, std::string>> g_cache;
constexpr auto kCacheTtl = std::chrono::seconds(3);

std::string cachedCapture(const std::string& cmd) {
    {
        std::lock_guard lk(g_cache_mu);
        auto it = g_cache.find(cmd);
        if (it != g_cache.end() && Clock::now() - it->second.first < kCacheTtl)
            return it->second.second;
    }
    // posix_spawn, not popen (popen deadlocks the threaded compositor).
    std::string out = spawnCapture(cmd);
    std::lock_guard lk(g_cache_mu);
    g_cache[cmd] = {Clock::now(), out};
    return out;
}

void invalidateCache() {
    std::lock_guard lk(g_cache_mu);
    g_cache.clear();
}

// Run a broker verb; log the outcome. Returns true on success.
bool brokerOp(const char* who, const char* verb, const std::string& arg) {
    int r = runPrivilegedOp(verb, arg);
    if (r == 0) {
        LOG_INFO("{}: {} '{}' via broker", who, verb, arg);
        invalidateCache();
        return true;
    }
    if (r == kBrokerUnreachable)
        LOG_INFO("{}: {} '{}' — broker unavailable (needs root)", who, verb, arg);
    else
        LOG_WARN("{}: {} '{}' failed ({})", who, verb, arg, r);
    return false;
}

// ── Services ─────────────────────────────────────────────────────────────────

constexpr const char* kServicesName = "com.freebsdx.Services";

std::vector<sysadmin::RcDaemon> enabledDaemons() {
    std::vector<sysadmin::RcDaemon> out;
    std::istringstream paths(cachedCapture("service -e 2>/dev/null"));
    std::string path;
    while (std::getline(paths, path)) {
        if (path.empty()) continue;
        std::ifstream f(path);
        if (!f) continue;
        std::stringstream body;
        body << f.rdbuf();
        size_t slash = path.rfind('/');
        sysadmin::RcDaemon d;
        if (sysadmin::parseRcScript(path.substr(slash + 1), body.str(), d))
            out.push_back(std::move(d));
    }
    return out;
}

shared::ServiceEntry serviceEntry(const sysadmin::RcDaemon& d,
                                  const std::vector<shared::ProcessEntry>& procs) {
    shared::ServiceEntry e{};
    std::strncpy(e.name_, d.name.c_str(), sizeof(e.name_) - 1);
    e.enabled_ = 1;                                   // only enabled scripts are listed
    for (const auto& p : procs) {
        if (sysadmin::commMatches(p.name_, d.procname)) { e.pid_ = p.pid_; break; }
    }
    std::strncpy(e.state_, e.pid_ ? "running" : "stopped", sizeof(e.state_) - 1);
    return e;
}

}  // namespace

void RcServicesService::handleMessage(const shared::XBusMessage& msg,
                                      std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::ServiceOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListServices: {
        auto daemons = enabledDaemons();
        auto procs = proc::enumerateProcesses();
        auto pg = makePage<shared::ServicePage>(readPage(msg), daemons,
            [&](shared::ServiceEntry& e, const sysadmin::RcDaemon& d) { e = serviceEntry(d, procs); });
        reply(msg, client, kServicesName, msg.opcode_, &pg);
        break;
    }
    case O::StartService:
    case O::StopService: {
        shared::ServiceRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.name_[sizeof(req.name_) - 1] = '\0';
        bool start = static_cast<O>(msg.opcode_) == O::StartService;
        bool ok = brokerOp("RcServicesService",
                           start ? privops::kServiceStart : privops::kServiceStop, req.name_);
        ack(msg, client, kServicesName);
        if (ok) {
            auto procs = proc::enumerateProcesses();
            for (const auto& d : enabledDaemons()) {
                if (d.name != req.name_) continue;
                auto e = serviceEntry(d, procs);
                broadcast(kServicesName, static_cast<uint32_t>(O::ServiceChanged), &e);
                break;
            }
        }
        break;
    }
    default: break;
    }
}

// ── Jails ────────────────────────────────────────────────────────────────────

void JailsService::handleMessage(const shared::XBusMessage& msg,
                                 std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::JailOpcode;
    constexpr const char* kName = "com.freebsdx.Jails";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListJails: {
        auto jails = sysadmin::parseJls(
            cachedCapture("jls -n jid host.hostname ip4.addr dying 2>/dev/null"));
        auto pg = makePage<shared::JailPage>(readPage(msg), jails,
            [](shared::JailEntry& e, const sysadmin::Jail& j) {
                e.jid_ = j.jid;
                std::strncpy(e.hostname_, j.hostname.c_str(), sizeof(e.hostname_) - 1);
                std::strncpy(e.ip_, j.ip.c_str(), sizeof(e.ip_) - 1);
                e.running_ = j.dying ? 0u : 1u;
            });
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::StopJail: {
        shared::JailStopRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        bool ok = req.jid_ != 0 &&
                  brokerOp("JailsService", privops::kJailRemove, std::to_string(req.jid_));
        ack(msg, client, kName);
        if (ok) broadcast<char>(kName, static_cast<uint32_t>(O::JailsChanged), nullptr);
        break;
    }
    default: break;
    }
}

// ── ZfsPools ─────────────────────────────────────────────────────────────────

void ZfsPoolsService::handleMessage(const shared::XBusMessage& msg,
                                    std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::ZfsOpcode;
    constexpr const char* kName = "com.freebsdx.ZfsPools";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListPools: {
        auto pools = sysadmin::parseZpoolList(
            cachedCapture("zpool list -Hp -o name,size,alloc,health 2>/dev/null"));
        auto pg = makePage<shared::ZfsPoolPage>(readPage(msg), pools,
            [](shared::ZfsPoolEntry& e, const sysadmin::Pool& p) {
                e.used_bytes_  = p.alloc_bytes;
                e.total_bytes_ = p.size_bytes;
                std::strncpy(e.name_, p.name.c_str(), sizeof(e.name_) - 1);
                std::strncpy(e.health_, p.health.c_str(), sizeof(e.health_) - 1);
                // Pool names come from zpool itself, but still only pass safe ones
                // to the command line.
                if (privops::safeName(p.name, 64, ".:")) {
                    uint32_t pct = 0;
                    e.scrubbing_ = sysadmin::parseScrubProgress(
                        cachedCapture("zpool status " + p.name + " 2>/dev/null"), pct) ? 1u : 0u;
                    e.scrub_pct_ = pct;
                }
            });
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::StartScrub: {
        shared::ZfsScrubRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.name_[sizeof(req.name_) - 1] = '\0';
        bool ok = brokerOp("ZfsPoolsService", privops::kZfsScrub, req.name_);
        ack(msg, client, kName);
        if (ok) broadcast<char>(kName, static_cast<uint32_t>(O::PoolsChanged), nullptr);
        break;
    }
    default: break;
    }
}

// ── BootEnv ──────────────────────────────────────────────────────────────────

void BootEnvService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::BeOpcode;
    constexpr const char* kName = "com.freebsdx.BootEnv";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListBE: {
        auto bes = sysadmin::parseBectlList(cachedCapture("bectl list -H 2>/dev/null"));
        auto pg = makePage<shared::BePage>(readPage(msg), bes,
            [](shared::BeEntry& e, const sysadmin::BootEnv& b) {
                std::strncpy(e.name_, b.name.c_str(), sizeof(e.name_) - 1);
                std::strncpy(e.created_, b.created.c_str(), sizeof(e.created_) - 1);
                e.active_    = b.active ? 1u : 0u;
                e.next_boot_ = b.next_boot ? 1u : 0u;
            });
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::ActivateBE:
    case O::DeleteBE: {
        shared::BeActionRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.name_[sizeof(req.name_) - 1] = '\0';
        bool destroy = static_cast<O>(msg.opcode_) == O::DeleteBE;
        bool ok = false;
        if (destroy) {
            // Never destroy the booted BE (bectl refuses too; check up front).
            bool booted = false;
            for (const auto& b : sysadmin::parseBectlList(cachedCapture("bectl list -H 2>/dev/null")))
                if (b.name == req.name_ && b.active) booted = true;
            if (booted) LOG_WARN("BootEnvService: refusing to destroy the booted BE '{}'", req.name_);
            else ok = brokerOp("BootEnvService", privops::kBeDestroy, req.name_);
        } else {
            ok = brokerOp("BootEnvService", privops::kBeActivate, req.name_);
        }
        ack(msg, client, kName);
        if (ok) broadcast<char>(kName, static_cast<uint32_t>(O::BEChanged), nullptr);
        break;
    }
    default: break;
    }
}

// ── NTP ──────────────────────────────────────────────────────────────────────

namespace {

shared::NtpState ntpState() {
    shared::NtpState st{};
    for (const auto& p : proc::enumerateProcesses())
        if (std::strcmp(p.name_, "ntpd") == 0) { st.enabled_ = 1; break; }
    if (!st.enabled_) return st;

    auto info = sysadmin::parseNtpqPeers(cachedCapture("timeout 3 ntpq -pn 2>/dev/null"));
    st.synced_ = info.synced ? 1u : 0u;
    std::strncpy(st.server_, info.server.c_str(), sizeof(st.server_) - 1);
    if (info.synced) {
        st.drift_ms_ = static_cast<int32_t>(std::lround(info.offset_ms));
        std::time_t t = std::time(nullptr) - static_cast<std::time_t>(info.when_sec);
        std::tm tm{};
        localtime_r(&t, &tm);
        std::strftime(st.last_sync_, sizeof(st.last_sync_), "%Y-%m-%d %H:%M:%S", &tm);
    }
    return st;
}

}  // namespace

void NtpService::handleMessage(const shared::XBusMessage& msg,
                               std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::NtpOpcode;
    constexpr const char* kName = "com.freebsdx.NTP";
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState: {
        auto st = ntpState();
        reply(msg, client, kName, msg.opcode_, &st);
        break;
    }
    case O::SetEnabled: {
        shared::NtpRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        brokerOp("NtpService", req.enabled_ ? privops::kServiceStart : privops::kServiceStop, "ntpd");
        ack(msg, client, kName);
        // Broadcast the real state either way, so a failed toggle snaps the
        // switch back instead of leaving it showing "Starting…".
        auto st = ntpState();
        broadcast(kName, static_cast<uint32_t>(O::StateChanged), &st);
        break;
    }
    default: break;
    }
}

// ── RemoteLogin ──────────────────────────────────────────────────────────────

namespace {

shared::RemoteLoginState remoteLoginState() {
    shared::RemoteLoginState st{};
    std::ifstream f("/etc/ssh/sshd_config");
    std::stringstream body;
    body << f.rdbuf();
    auto cfg = sysadmin::parseSshdConfig(body.str());
    st.port_ = cfg.port;
    std::strncpy(st.bind_addr_, cfg.listen.c_str(), sizeof(st.bind_addr_) - 1);
    for (const auto& p : proc::enumerateProcesses())
        if (std::strcmp(p.name_, "sshd") == 0) { st.enabled_ = 1; break; }
    if (st.enabled_)
        st.sessions_ = sysadmin::countEstablished(cachedCapture("netstat -an -p tcp 2>/dev/null"), cfg.port);
    return st;
}

}  // namespace

void RemoteLoginService::handleMessage(const shared::XBusMessage& msg,
                                       std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::RemoteLoginOpcode;
    constexpr const char* kName = "com.freebsdx.RemoteLogin";
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState: {
        auto st = remoteLoginState();
        reply(msg, client, kName, msg.opcode_, &st);
        break;
    }
    case O::SetEnabled: {
        shared::RemoteLoginRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        brokerOp("RemoteLoginService", req.enabled_ ? privops::kServiceStart : privops::kServiceStop, "sshd");
        ack(msg, client, kName);
        // Real state either way, so a failed toggle snaps the switch back.
        auto st = remoteLoginState();
        broadcast(kName, static_cast<uint32_t>(O::StateChanged), &st);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
