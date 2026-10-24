#include "xserver/portal_service.h"
#include "xserver/ipc_socket.h"
#include "xserver/shell_util.h"
#include "xserver/async_shell.h"
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <ctime>
#include <string>

namespace fbx::xserver {

bool PortalService::ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.Portal") != 0) return false;

    if (msg.opcode_ == static_cast<uint32_t>(shared::PortalOpcode::OpenFile)) {
        handleOpenFile(client, msg);
        return true;
    }

    if (msg.opcode_ == static_cast<uint32_t>(shared::PortalOpcode::ListDir)) {
        handleListDir(client, msg);
        return true;
    }

    if (msg.opcode_ == static_cast<uint32_t>(shared::PortalOpcode::ZfsRollback)) {
        handleZfsRollback(client, msg);
        return true;
    }

    if (msg.opcode_ == static_cast<uint32_t>(shared::PortalOpcode::ZfsSnapshot)) {
        handleZfsSnapshot(client, msg);
        return true;
    }

    return false;
}

void PortalService::handleOpenFile(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept {
    // Path is a null-terminated string in the first bytes of the payload
    const char* requested_path = reinterpret_cast<const char*>(msg.payload_);

    // Validate: only allow paths under $HOME or /tmp
    const char* home = std::getenv("HOME");
    bool allowed = (std::strncmp(requested_path, "/tmp/", 5) == 0);
    if (!allowed && home) {
        size_t home_len = std::strlen(home);
        allowed = (std::strncmp(requested_path, home, home_len) == 0 &&
                   (requested_path[home_len] == '/' || requested_path[home_len] == '\0'));
    }

    shared::XBusMessage response = msg;

    if (!allowed) {
        std::cout << "[PortalService] Access DENIED for path: " << requested_path << "\n";
        std::strncpy(reinterpret_cast<char*>(response.payload_), "DENIED", sizeof(response.payload_) - 1);
        std::span<const uint8_t> resp_span(reinterpret_cast<const uint8_t*>(&response), sizeof(response));
        (void)client->sendMessage(resp_span);
        return;
    }

    std::cout << "[PortalService] Secure file request for: " << requested_path << "\n";
    int fd = open(requested_path, O_RDONLY);
    if (fd >= 0) {
        std::strncpy(reinterpret_cast<char*>(response.payload_), "SUCCESS", sizeof(response.payload_) - 1);
        std::span<const uint8_t> resp_span(reinterpret_cast<const uint8_t*>(&response), sizeof(response));
        (void)client->sendMessage(resp_span, fd);
        close(fd);
        std::cout << "[PortalService] FD handed to client. Access granted.\n";
    } else {
        std::strncpy(reinterpret_cast<char*>(response.payload_), "ERROR", sizeof(response.payload_) - 1);
        std::span<const uint8_t> resp_span(reinterpret_cast<const uint8_t*>(&response), sizeof(response));
        (void)client->sendMessage(resp_span);
    }
}

void PortalService::handleListDir(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept {
    shared::DirListRequest req{};
    std::memcpy(&req, msg.payload_, sizeof(req));
    req.path_[sizeof(req.path_) - 1] = '\0';

    const char* home = std::getenv("HOME");
    bool allowed = (std::strncmp(req.path_, "/tmp", 4) == 0);
    if (!allowed && home) {
        size_t hlen = std::strlen(home);
        // Allow $HOME and all subdirectories, including .zfs/snapshot virtual dirs.
        allowed = (std::strncmp(req.path_, home, hlen) == 0 &&
                   (req.path_[hlen] == '/' || req.path_[hlen] == '\0'));
    }

    shared::XBusMessage response = msg;
    shared::DirListing listing{};

    if (!allowed) {
        std::cerr << "[PortalService] ListDir DENIED: " << req.path_ << "\n";
        std::memcpy(response.payload_, &listing, sizeof(listing));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        return;
    }

    // Collect all entries (dirs first, then files, both alphabetical)
    struct RawEntry { std::string name; bool is_dir; };
    std::vector<RawEntry> all;
    DIR* dp = opendir(req.path_);
    if (dp) {
        struct dirent* de;
        while ((de = readdir(dp)) != nullptr) {
            if (de->d_name[0] == '.') continue; // skip hidden + . and ..
            bool is_dir = (de->d_type == DT_DIR);
            if (de->d_type == DT_UNKNOWN) {
                // Fall back to stat for filesystems that don't populate d_type
                std::string full = std::string(req.path_, strnlen(req.path_, sizeof(req.path_))) + "/" + de->d_name;
                struct stat st{};
                if (stat(full.c_str(), &st) == 0) is_dir = S_ISDIR(st.st_mode);
            }
            all.push_back({de->d_name, is_dir});
        }
        closedir(dp);
    }
    std::sort(all.begin(), all.end(), [](const RawEntry& a, const RawEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir; // dirs first
        return a.name < b.name;
    });

    constexpr uint32_t kPageSize = 4;
    uint32_t total = static_cast<uint32_t>(all.size());
    uint32_t page  = req.page_;
    uint32_t start = page * kPageSize;
    uint32_t count = (start < total) ? std::min(kPageSize, total - start) : 0u;

    listing.entry_count_ = count;
    listing.total_count_ = total;
    listing.page_        = page;
    listing.has_more_    = ((start + count) < total) ? 1u : 0u;

    for (uint32_t i = 0; i < count; ++i) {
        const auto& e = all[start + i];
        std::strncpy(listing.entries_[i].name_, e.name.c_str(), sizeof(listing.entries_[i].name_) - 1);
        listing.entries_[i].is_dir_ = e.is_dir ? 1u : 0u;
    }

    std::memcpy(response.payload_, &listing, sizeof(listing));
    (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
    std::cout << "[PortalService] ListDir " << req.path_ << " page=" << page
              << " count=" << count << "/" << total << "\n";
}

// ── ZFS helpers ──────────────────────────────────────────────────────────────

// Given an absolute path inside a ZFS mountpoint (or a .zfs/snapshot/* path),
// derive the dataset name by querying `zfs list -H -o name <mountpoint>`.
// Returns empty string on failure.
static std::string datasetForPath(const std::string& path) {
    // Walk up the path to find the ZFS mountpoint by running zfs list.
    // Use popen for simplicity; this runs server-side with full privileges.
    std::string p = path;
    // Strip any .zfs/snapshot/... suffix to get the real mountpoint root
    auto zfs_pos = p.find("/.zfs/");
    if (zfs_pos != std::string::npos) p = p.substr(0, zfs_pos);

    // posix_spawn, not popen (fork-safe in the multithreaded compositor — see
    // async_shell.h), then split the captured output into lines.
    std::string listing = fbx::xserver::spawnCapture("zfs list -H -o name,mountpoint 2>/dev/null");

    std::string best_dataset;
    size_t best_len = 0;
    std::istringstream iss(listing);
    std::string line;
    while (std::getline(iss, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string dataset = line.substr(0, tab);
        std::string mountpoint = line.substr(tab + 1);  // getline already stripped '\n'
        if (mountpoint.empty() || mountpoint == "none" || mountpoint == "-") continue;
        // Check if path starts with this mountpoint
        if (p.size() >= mountpoint.size() &&
            p.compare(0, mountpoint.size(), mountpoint) == 0 &&
            (p.size() == mountpoint.size() || p[mountpoint.size()] == '/')) {
            if (mountpoint.size() > best_len) {
                best_len = mountpoint.size();
                best_dataset = dataset;
            }
        }
    }
    return best_dataset;
}

static void sendZfsResponse(std::shared_ptr<fbx::xserver::IpcSocket> client,
                             const fbx::shared::XBusMessage& msg,
                             bool success, const std::string& message) {
    fbx::shared::XBusMessage response = msg;
    fbx::shared::ZfsActionResponse resp{};
    resp.success_ = success ? 1u : 0u;
    std::strncpy(resp.message_, message.c_str(), sizeof(resp.message_) - 1);
    std::memcpy(response.payload_, &resp, sizeof(resp));
    (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
}

void PortalService::handleZfsRollback(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept {
    shared::ZfsActionRequest req{};
    std::memcpy(&req, msg.payload_, sizeof(req));
    req.path_[sizeof(req.path_) - 1] = '\0';
    std::string snap_path = req.path_;

    // Validate: must be under $HOME
    const char* home = std::getenv("HOME");
    bool allowed = false;
    if (home) {
        size_t hlen = std::strlen(home);
        allowed = (snap_path.size() >= hlen &&
                   snap_path.compare(0, hlen, home) == 0);
    }
    if (!allowed) {
        sendZfsResponse(client, msg, false, "Access denied");
        return;
    }

    // Expect path of form: <mountpoint>/.zfs/snapshot/<snapname>
    auto snap_marker = snap_path.find("/.zfs/snapshot/");
    if (snap_marker == std::string::npos) {
        sendZfsResponse(client, msg, false, "Not a snapshot path");
        return;
    }

    std::string mountpoint = snap_path.substr(0, snap_marker);
    std::string snapname   = snap_path.substr(snap_marker + 15); // len("/.zfs/snapshot/") = 15

    std::string dataset = datasetForPath(mountpoint);
    if (dataset.empty()) {
        sendZfsResponse(client, msg, false, "Could not determine ZFS dataset");
        return;
    }

    if (!isValidZfsName(dataset) || !isValidZfsName(snapname)) {
        sendZfsResponse(client, msg, false, "Invalid dataset or snapshot name");
        return;
    }
    std::string snap_spec = dataset + "@" + snapname;
    // -r: recursively destroy intermediate snapshots so rollback can proceed
    // snap_spec ends in a client-supplied snapshot name — quote it.
    std::string cmd = "zfs rollback -r " + shellQuote(snap_spec) + " 2>&1";
    std::cout << "[PortalService] ZfsRollback: " << cmd << "\n";

    int rc = -1;
    std::string output = fbx::xserver::spawnCaptureStatus(cmd, &rc);  // posix_spawn, fork-safe
    if (rc == 0) {
        sendZfsResponse(client, msg, true, "Rolled back to " + snapname);
    } else {
        if (output.empty()) output = "zfs rollback failed";
        sendZfsResponse(client, msg, false, output);
    }
}

void PortalService::handleZfsSnapshot(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept {
    shared::ZfsActionRequest req{};
    std::memcpy(&req, msg.payload_, sizeof(req));
    req.path_[sizeof(req.path_) - 1] = '\0';
    std::string dir_path = req.path_;

    // Validate: must be under $HOME
    const char* home = std::getenv("HOME");
    bool allowed = false;
    if (home) {
        size_t hlen = std::strlen(home);
        allowed = (dir_path.size() >= hlen &&
                   dir_path.compare(0, hlen, home) == 0);
    }
    if (!allowed) {
        sendZfsResponse(client, msg, false, "Access denied");
        return;
    }

    std::string dataset = datasetForPath(dir_path);
    if (dataset.empty()) {
        sendZfsResponse(client, msg, false, "Could not determine ZFS dataset");
        return;
    }

    // Generate timestamp-based snapshot name
    std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H%M%S", std::localtime(&now));
    if (!isValidZfsName(dataset)) {
        sendZfsResponse(client, msg, false, "Invalid dataset name");
        return;
    }
    std::string snap_spec = dataset + "@snap-" + ts;

    std::string cmd = "zfs snapshot " + shellQuote(snap_spec) + " 2>&1";
    std::cout << "[PortalService] ZfsSnapshot: " << cmd << "\n";

    int rc = -1;
    std::string output = fbx::xserver::spawnCaptureStatus(cmd, &rc);  // posix_spawn, fork-safe
    if (rc == 0) {
        sendZfsResponse(client, msg, true, "Snapshot created: snap-" + std::string(ts));
    } else {
        if (output.empty()) output = "zfs snapshot failed";
        sendZfsResponse(client, msg, false, output);
    }
}

} // namespace fbx::xserver
