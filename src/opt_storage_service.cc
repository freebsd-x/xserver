#include "xserver/opt_storage_service.h"
#include "xserver/config_service.h"
#include "xserver/ipc_socket.h"
#include "xserver/user_files.h"
#include "xserver/xbus_reply.h"
#include "shared/logger.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace fbx::xserver {

std::atomic<uint32_t> OptStorageService::reclaimable_mb_{0};
std::atomic<bool>     OptStorageService::busy_{false};
std::atomic<bool>     OptStorageService::clean_requested_{false};

namespace {

using namespace svc;
namespace fs = std::filesystem;

uint32_t cfg(const char* key, uint32_t def) {
    return static_cast<uint32_t>(std::strtoul(ConfigService::get(key, std::to_string(def)).c_str(), nullptr, 10));
}

shared::OptStorageState state() {
    shared::OptStorageState st{};
    st.enabled_        = cfg("optstorage.enabled", 0) ? 1u : 0u;
    st.target_pct_     = std::min(cfg("optstorage.target", 15), 100u);
    st.reclaimable_mb_ = 0;
    return st;
}

// Free space on the home filesystem, in percent (100 if unknown).
uint32_t freePct() {
    struct statvfs v{};
    if (::statvfs(userfiles::homeDir().c_str(), &v) != 0 || v.f_blocks == 0) return 100;
    return static_cast<uint32_t>(100.0 * static_cast<double>(v.f_bavail) / static_cast<double>(v.f_blocks));
}

}  // namespace

void OptStorageService::broadcastState() noexcept {
    auto st = state();
    st.reclaimable_mb_ = reclaimable_mb_.load();
    broadcast("com.freebsdx.OptStorage", static_cast<uint32_t>(shared::OptStorageOpcode::StateChanged), &st);
}

// Walk ~/.cache on a worker: total the stale files, deleting them if a clean
// was asked for. One worker at a time; a clean requested while it runs is kept
// and done in another pass, never dropped.
void OptStorageService::startScan(bool clean) noexcept {
    if (clean) clean_requested_ = true;
    if (busy_.exchange(true)) return;
    std::thread([] {
        for (;;) {
            const bool clean = clean_requested_.exchange(false);
            const std::string root = userfiles::homeDir() + "/.cache";
            const std::time_t now = std::time(nullptr);
            uint64_t stale_bytes = 0, freed = 0;
            std::error_code ec;
            fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end; it.increment(ec)) {
                struct stat st{};
                if (::lstat(it->path().c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;   // no symlinks
                if (!isStale(st.st_mtime, st.st_atime, now)) continue;
                if (clean && ::unlink(it->path().c_str()) == 0) { freed += static_cast<uint64_t>(st.st_size); continue; }
                stale_bytes += static_cast<uint64_t>(st.st_size);
            }
            if (clean) LOG_INFO("OptStorage: cleared {} MB of old cache files", freed >> 20);
            reclaimable_mb_ = static_cast<uint32_t>(stale_bytes >> 20);
            broadcastState();
            busy_ = false;
            // A clean asked for after we read the flag: go again (unless
            // another worker already took over).
            if (!clean_requested_ || busy_.exchange(true)) break;
        }
    }).detach();
}

void OptStorageService::poll() noexcept {
    static auto last = std::chrono::steady_clock::time_point{};
    auto now = std::chrono::steady_clock::now();
    if (last != std::chrono::steady_clock::time_point{} && now - last < std::chrono::hours(1)) return;
    last = now;
    auto st = state();
    bool clean = st.enabled_ && freePct() < st.target_pct_;
    startScan(clean);
}

bool OptStorageService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                       const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.OptStorage") != 0) return false;
    using Op = shared::OptStorageOpcode;
    constexpr const char* kName = "com.freebsdx.OptStorage";
    switch (static_cast<Op>(msg.opcode_)) {
    case Op::GetState: {
        auto st = state();
        st.reclaimable_mb_ = reclaimable_mb_.load();
        reply(msg, client, kName, msg.opcode_, &st);
        startScan(false);   // refresh the measurement; StateChanged follows
        break;
    }
    case Op::SetState: {
        shared::OptStorageRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        ConfigService::set("optstorage.enabled", req.enabled_ ? "1" : "0");
        ConfigService::set("optstorage.target", std::to_string(std::min(req.target_pct_, 100u)));
        auto st = state();
        LOG_INFO("OptStorage: enabled={} target={}% (home free {}%)", st.enabled_, st.target_pct_, freePct());
        ack(msg, client, kName);
        startScan(st.enabled_ && freePct() < st.target_pct_);
        break;
    }
    default:
        ack(msg, client, kName);
        break;
    }
    return true;
}

} // namespace fbx::xserver
