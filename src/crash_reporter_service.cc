#include "xserver/crash_reporter_service.h"
#include "shared/logger.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace fbx::xserver {
namespace {

struct Rec {
    std::string path;
    std::string app;
    std::string signal;
    uint64_t    ts = 0;    // Unix epoch seconds (file mtime)
    uint32_t    id = 0;    // stable handle for DeleteReport
};

std::string spoolDir() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.freebsdx/crashes";
}

// Stable per-file id so DeleteReport(report_id) can find the file again.
uint32_t idOf(const std::string& filename) {
    uint32_t h = 2166136261u;  // FNV-1a/32
    for (unsigned char c : filename) { h ^= c; h *= 16777619u; }
    return h ? h : 1u;  // never 0 (0 reads as "unset")
}

// Read the "=== FreeBSD-X crash: SIGSEGV @ ... ===" header line.
std::string parseSignal(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto c = line.find("crash:");
        if (c == std::string::npos) continue;
        std::string rest = line.substr(c + 6);
        char sig[16] = {0};
        std::sscanf(rest.c_str(), " %15s", sig);
        return sig[0] ? sig : "?";
    }
    return "?";
}

std::vector<Rec> scan() {
    std::vector<Rec> out;
    std::error_code ec;
    const std::string dir = spoolDir();
    if (!fs::exists(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".crash") continue;
        Rec r;
        r.path = e.path().string();
        std::string stem = e.path().stem().string();          // "<proc>-<pid>"
        if (auto dash = stem.rfind('-'); dash != std::string::npos)
            stem.erase(dash);                                 // strip -<pid>
        r.app = stem;
        r.signal = parseSignal(r.path);
        r.id = idOf(e.path().filename().string());
        auto ft = fs::last_write_time(e.path(), ec);
        if (!ec) {
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            r.ts = static_cast<uint64_t>(
                std::chrono::system_clock::to_time_t(sctp));
        }
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(),
              [](const Rec& a, const Rec& b) { return a.ts > b.ts; });  // newest first
    return out;
}

void sendPage(IpcSocket* client, uint32_t tid, uint32_t page) noexcept {
    const auto recs = scan();
    shared::CrashReportPage pg{};
    pg.page_  = page;
    pg.total_ = static_cast<uint32_t>(recs.size());

    const uint32_t start = page * 3;
    uint32_t n = 0;
    for (uint32_t i = start; i < recs.size() && n < 3; ++i, ++n) {
        const Rec& r = recs[i];
        shared::CrashReport& e = pg.entries_[n];
        std::strncpy(e.app_,    r.app.c_str(),    sizeof(e.app_) - 1);
        std::strncpy(e.signal_, r.signal.c_str(), sizeof(e.signal_) - 1);
        e.timestamp_ = r.ts;
        e.report_id_ = r.id;
    }
    pg.count_    = n;
    pg.has_more_ = (start + n < recs.size()) ? 1u : 0u;

    shared::XBusMessage reply{};
    reply.transaction_id_ = tid;   // echo so the client's Call callback matches
    std::strncpy(reply.service_name_, "com.freebsdx.CrashReporter",
                 sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(shared::CrashOpcode::ListReports);
    std::memcpy(reply.payload_, &pg, sizeof(pg));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

void deleteReport(uint32_t report_id) noexcept {
    std::error_code ec;
    for (const Rec& r : scan()) {
        if (r.id == report_id) {
            fs::remove(r.path, ec);
            LOG_INFO("CrashReporterService: deleted report {} ({})", report_id, r.path);
            return;
        }
    }
}

void clearAll() noexcept {
    std::error_code ec;
    int n = 0;
    for (const Rec& r : scan()) if (fs::remove(r.path, ec)) ++n;
    LOG_INFO("CrashReporterService: cleared {} report(s)", n);
}

}  // namespace

void CrashReporterService::handleMessage(const shared::XBusMessage& msg,
                                         std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::CrashOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListReports: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));
        sendPage(client.get(), msg.transaction_id_, page);
        break;
    }
    case O::DeleteReport: {
        shared::CrashDeleteRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        deleteReport(req.report_id_);
        break;
    }
    case O::ClearAll:
        clearAll();
        break;
    default:
        break;
    }
}

}  // namespace fbx::xserver
