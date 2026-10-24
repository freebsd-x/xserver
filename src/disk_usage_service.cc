#include "xserver/disk_usage_service.h"

#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <thread>
#include <filesystem>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
namespace fs = std::filesystem;

std::mutex                         g_mutex;
std::vector<shared::DiskUsageEntry> g_results;   // sorted desc by size
std::atomic<bool>                  g_scanning{false};

constexpr std::size_t kMaxEntries = 512;

// Recursive byte total of a directory subtree; skips symlinks (no loops) and
// unreadable entries. Best-effort — errors are ignored, not propagated.
uint64_t subtreeSize(const fs::path& root) {
    uint64_t total = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        std::error_code e2;
        if (it->is_symlink(e2)) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(e2)) {
            auto s = it->file_size(e2);
            if (!e2) total += s;
        }
    }
    return total;
}

void broadcastComplete() {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, "com.freebsdx.DiskUsage", sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::DiskUsageOpcode::ScanComplete);
    BroadcastXBusMessage(b);
}

void replyPage(std::shared_ptr<IpcSocket>& client, uint32_t tid, uint32_t page) {
    shared::DiskUsagePage pg{};
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        uint32_t total = static_cast<uint32_t>(g_results.size());
        uint32_t start = page * 3u;
        uint32_t count = 0;
        for (; count < 3u && start + count < total; ++count)
            pg.entries_[count] = g_results[start + count];
        pg.page_     = page;
        pg.total_    = total;
        pg.count_    = count;
        pg.has_more_ = (start + count < total) ? 1u : 0u;
    }
    if (!client) return;
    shared::XBusMessage m{};
    m.transaction_id_ = tid;
    std::strncpy(m.service_name_, "com.freebsdx.DiskUsage", sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(shared::DiskUsageOpcode::GetResults);
    std::memcpy(m.payload_, &pg, sizeof(pg));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

}  // namespace

void DiskUsageService::scanNow(const std::string& path) noexcept {
    std::vector<shared::DiskUsageEntry> res;
    uint64_t root_total = 0;
    std::error_code ec;
    fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end && res.size() < kMaxEntries; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        std::error_code e2;
        uint64_t sz = 0;
        if (it->is_symlink(e2)) continue;                     // don't traverse links
        if (it->is_directory(e2))          sz = subtreeSize(it->path());
        else if (it->is_regular_file(e2)) { auto s = it->file_size(e2); if (!e2) sz = s; }
        else continue;
        shared::DiskUsageEntry e{};
        std::strncpy(e.path_, it->path().string().c_str(), sizeof(e.path_) - 1);
        e.size_bytes_ = sz;
        res.push_back(e);
        root_total += sz;
    }
    std::sort(res.begin(), res.end(),
              [](const auto& a, const auto& b) { return a.size_bytes_ > b.size_bytes_; });
    for (auto& e : res)
        e.pct_of_total_ = root_total ? static_cast<uint32_t>(e.size_bytes_ * 100 / root_total) : 0;

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_results = std::move(res);
    }
    g_scanning = false;
}

uint32_t DiskUsageService::resultCount() noexcept {
    std::lock_guard<std::mutex> lk(g_mutex);
    return static_cast<uint32_t>(g_results.size());
}

void DiskUsageService::handleMessage(const shared::XBusMessage& msg,
                                     std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::DiskUsageOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::Scan: {
        shared::DiskUsageScanRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.path_[sizeof(req.path_) - 1] = '\0';
        std::string path = req.path_[0] ? std::string(req.path_) : std::string("/");
        bool expected = false;
        if (g_scanning.compare_exchange_strong(expected, true)) {
            std::thread([path]() { scanNow(path); broadcastComplete(); }).detach();
        }
        break;   // async; results delivered via GetResults after ScanComplete
    }
    case O::GetResults: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));   // caller sends uint32_t page
        replyPage(client, msg.transaction_id_, page);
        break;
    }
    default:
        break;
    }
}

}  // namespace fbx::xserver
