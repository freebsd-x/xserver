#include "xserver/adblock_service.h"
#include "xserver/adblock.h"
#include "xserver/async_shell.h"
#include "xserver/config_service.h"
#include "xserver/priv_broker.h"
#include "xserver/priv_ops.h"
#include "xserver/user_files.h"
#include "xserver/xbus_reply.h"
#include "shared/logger.h"

#include <atomic>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fbx::xserver {

namespace {

using namespace svc;
constexpr const char* kSvc = "com.freebsdx.AdBlock";

std::mutex                g_backend_mu;
AdBlockService::Backend   g_backend;
std::atomic<bool>         g_updating{false};

AdBlockService::Backend backend() {
    std::lock_guard lk(g_backend_mu);
    AdBlockService::Backend b = g_backend;
    if (!b.download)
        b.download = [](const std::string& url, const std::string& dest) {
            // fetch(1), argv only (no shell); -T bounds a stalled transfer.
            return spawnArgvStatus({"/usr/bin/fetch", "-q", "-T", "60", "-o", dest, url}) == 0;
        };
    if (!b.apply)
        b.apply = [](bool on) { return runPrivilegedOp(privops::kAdBlockApply, on ? "1" : "0"); };
    return b;
}

std::string listDir()  {
    std::string d = userfiles::homeDir();
    for (const char* c : adblock::kListDirs) { d += '/'; d += c; }
    return d;
}
std::string listPath() { return listDir() + "/" + adblock::kListFile; }

// The configured list URL: https only (it goes to fetch as one argv element).
std::string listUrl() {
    std::string u = ConfigService::get("adblock.list_url", adblock::kDefaultListUrl);
    bool ok = u.starts_with("https://") && u.size() < 512 &&
              u.find_first_of(" \t\r\n") == std::string::npos;
    return ok ? u : adblock::kDefaultListUrl;
}

// What /etc/hosts holds right now (re-read only when it changes).
size_t blockedNow(const std::string& hosts_path) {
    static std::mutex mu;
    static struct timespec cached_mtime{};
    static off_t cached_size = -1;
    static size_t cached_count = 0;
    struct stat st{};
    if (::stat(hosts_path.c_str(), &st) != 0) return 0;
    std::lock_guard lk(mu);
    if (st.st_size != cached_size || st.st_mtim.tv_sec != cached_mtime.tv_sec ||
        st.st_mtim.tv_nsec != cached_mtime.tv_nsec) {
        cached_count = adblock::blockedCount(userfiles::readFile(hosts_path).value_or(""));
        cached_size  = st.st_size;
        cached_mtime = st.st_mtim;
    }
    return cached_count;
}

shared::AdBlockStatus build() {
    shared::AdBlockStatus st{};
    size_t blocked = blockedNow(backend().hosts_path);
    st.enabled_ = blocked > 0 ? 1u : 0u;
    st.domain_count_ = static_cast<uint32_t>(
        blocked > 0 ? blocked : std::strtoul(ConfigService::get("adblock.list_count", "0").c_str(), nullptr, 10));
    std::string date = ConfigService::get("adblock.list_date", "");
    std::strncpy(st.list_updated_, date.c_str(), sizeof(st.list_updated_) - 1);
    st.updating_ = g_updating.load() ? 1u : 0u;
    return st;
}

void bcast() {
    auto st = build();
    broadcast(kSvc, static_cast<uint32_t>(shared::AdBlockOpcode::StatusChanged), &st);
}

// Download the list and store it normalized. One download at a time.
bool updateList() {
    if (g_updating.exchange(true)) return false;   // already running
    bcast();                                       // "Updating…"
    const auto b = backend();
    std::error_code ec;
    std::filesystem::create_directories(listDir(), ec);
    const std::string tmp = listDir() + "/download.tmp";
    bool ok = false;
    if (!b.download(listUrl(), tmp)) {
        LOG_WARN("AdBlock: couldn't download the blocklist from {}", listUrl());
    } else if (auto raw = userfiles::readFile(tmp); !raw || raw->size() > adblock::kMaxListBytes) {
        LOG_WARN("AdBlock: the downloaded blocklist is missing or too large");
    } else if (auto domains = adblock::parseList(*raw); domains.empty()) {
        LOG_WARN("AdBlock: the downloaded blocklist has no usable entries");
    } else if (!userfiles::writeFileAtomic(listPath(), adblock::formatList(domains), 0644)) {
        LOG_WARN("AdBlock: couldn't save the blocklist to {}", listPath());
    } else {
        ConfigService::set("adblock.list_date", adblock::formatListDate(std::time(nullptr)));
        ConfigService::set("adblock.list_count", std::to_string(domains.size()));
        LOG_INFO("AdBlock: blocklist updated ({} domains)", domains.size());
        ok = true;
    }
    ::unlink(tmp.c_str());
    g_updating = false;
    return ok;
}

bool applyHosts(bool on) {
    int rc = backend().apply(on);
    if (rc != 0)
        LOG_WARN("AdBlock: couldn't {} the /etc/hosts block ({}){}", on ? "write" : "remove", rc,
                 rc == -EINVAL ? " — reinstall xsessiond (sudo sh scripts/xsessiond-install.sh) for adblock.apply" : "");
    return rc == 0;
}

}  // namespace

void AdBlockService::setBackend(Backend b) noexcept {
    std::lock_guard lk(g_backend_mu);
    g_backend = std::move(b);
}

void AdBlockService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::AdBlockOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetStatus: {
        auto st = build();
        reply(msg, client, kSvc, msg.opcode_, &st);
        break;
    }
    case O::SetEnabled: {
        shared::AdBlockEnableRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        const bool on = req.enabled_ != 0;
        // First time on: there's no list yet, so fetch one.
        if (on && !std::filesystem::exists(listPath())) updateList();
        if (!on || std::filesystem::exists(listPath())) applyHosts(on);
        bcast();   // the real state, so the switch reflects what happened
        break;
    }
    case O::UpdateList: {
        // A fresh list goes live right away if AdBlock is on.
        if (updateList() && blockedNow(backend().hosts_path) > 0) applyHosts(true);
        bcast();
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
