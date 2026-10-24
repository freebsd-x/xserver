// disk_service.cc — com.freebsdx.Disk
//
// Wraps FreeBSD geom/gpart/zpool/mount CLI tools to enumerate disks,
// list partitions, mount/unmount/format/eject, and query ZFS pool status.
// All popen() arguments are validated to prevent shell injection.

#include "xserver/disk_service.h"
#include "xserver/shell_util.h"
#include "xserver/async_shell.h"

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }
using fbx::xserver::BroadcastXBusMessage;

#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace fbx {

using namespace shared;

std::mutex DiskService::mutex_;

// ── Input validation ──────────────────────────────────────────────────────────

bool DiskService::isValidDevice(const std::string& dev) noexcept {
    // Accepts: "ada0", "da1", "nvd0p2", "mmcsd0s1" etc.
    if (dev.empty() || dev.size() > 32) return false;
    if (!std::isalpha(static_cast<unsigned char>(dev[0]))) return false;
    for (char c : dev)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.')
            return false;
    return true;
}

bool DiskService::isValidPath(const std::string& path) noexcept {
    if (path.empty() || path.size() > 128 || path[0] != '/') return false;
    for (char c : path)
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '/' && c != '_' && c != '.' && c != '-')
            return false;
    return true;
}

// ── Size helpers ──────────────────────────────────────────────────────────────

// Parse "(200M)" / "(1.0G)" / "(32G)" → bytes
static uint64_t parseHumanSize(const std::string& s) noexcept {
    if (s.size() < 3) return 0;
    std::string inner = (s[0] == '(') ? s.substr(1, s.size() - 2) : s;
    if (inner.empty()) return 0;
    char unit = inner.back();
    double val = 0.0;
    try { val = std::stod(inner.substr(0, inner.size() - 1)); }
    catch (...) { return 0; }
    switch (unit) {
        case 'K': return static_cast<uint64_t>(val * 1024.0);
        case 'M': return static_cast<uint64_t>(val * 1024.0 * 1024.0);
        case 'G': return static_cast<uint64_t>(val * 1024.0 * 1024.0 * 1024.0);
        case 'T': return static_cast<uint64_t>(val * 1024.0 * 1024.0 * 1024.0 * 1024.0);
        default:  return static_cast<uint64_t>(val);
    }
}

// ── Disk enumeration ──────────────────────────────────────────────────────────

std::vector<DiskInfo> DiskService::enumerateDisks() noexcept {
    std::vector<DiskInfo> disks;

    // Bounded so a wedged GEOM tasting can't hang the event loop forever.
    std::string output = xserver::runCapped("geom disk list 2>/dev/null");

    DiskInfo cur{};
    bool in_disk = false;

    std::istringstream ss(output);
    std::string line;
    while (std::getline(ss, line)) {
        auto trim = [](std::string s) {
            while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
            while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
            return s;
        };

        if (line.rfind("Geom name:", 0) == 0) {
            if (in_disk) disks.push_back(cur);
            cur = {};
            auto name = trim(line.substr(10));
            std::strncpy(cur.name_, name.c_str(), sizeof(cur.name_) - 1);
            in_disk = true;
        } else if (in_disk) {
            auto pos = line.find(':');
            if (pos == std::string::npos) continue;
            auto key = trim(line.substr(0, pos));
            auto val = trim(line.substr(pos + 1));

            if (key == "Mediasize") {
                // "500107862016 (465G)"
                try { cur.size_bytes_ = std::stoull(val); } catch (...) {}
            } else if (key == "descr") {
                std::strncpy(cur.model_, val.c_str(), sizeof(cur.model_) - 1);
            } else if (key == "ident") {
                std::strncpy(cur.serial_, val.c_str(), sizeof(cur.serial_) - 1);
            } else if (key == "rotationrate") {
                // 0 = SSD, unknown = removable/unknown, >0 = HDD
                cur.is_removable_ = (val == "unknown") ? 1 : 0;
            }
        }
    }
    if (in_disk) disks.push_back(cur);
    return disks;
}

// ── Partition enumeration ─────────────────────────────────────────────────────

std::vector<PartitionInfo> DiskService::listPartitions(const std::string& disk) noexcept {
    std::vector<PartitionInfo> parts;

    // Get mount info: device → mountpoint
    std::map<std::string, std::string> mounts;
    {
        std::istringstream mss(xserver::runCapped("mount 2>/dev/null"));
        std::string mline;
        while (std::getline(mss, mline)) {
            // "/dev/ada0p3 on / (ufs, local, ...)"
            auto on_pos = mline.find(" on ");
            if (on_pos == std::string::npos) continue;
            std::string dev = mline.substr(0, on_pos);
            if (dev.rfind("/dev/", 0) == 0) dev = dev.substr(5);
            auto paren = mline.find(" (", on_pos + 4);
            if (paren == std::string::npos) continue;
            std::string mnt = mline.substr(on_pos + 4, paren - on_pos - 4);
            mounts[dev] = mnt;
        }
    }

    // gpart show -p: full provider names (bounded so it can't hang the loop)
    std::string cmd = "gpart show -p " + disk + " 2>/dev/null";
    std::istringstream gss(xserver::runCapped(cmd));
    std::string line;
    while (std::getline(gss, line)) {
        // Header: "=>     40  976773088  ada0  GPT  (465G)"
        if (!line.empty() && line[0] == '=') continue;
        // Skip blank lines
        bool all_space = true;
        for (char c : line) if (!std::isspace((unsigned char)c)) { all_space = false; break; }
        if (all_space) continue;

        // Partition: "         40     409600  ada0p1  efi  (200M)"
        std::istringstream iss(line);
        uint64_t start_sec, size_sec;
        std::string name, type, human;
        if (!(iss >> start_sec >> size_sec >> name >> type)) continue;
        iss >> human; // "(200M)" — optional

        // Skip the disk itself (no 'p' in name means it's the disk header entry)
        if (name == disk) continue;

        PartitionInfo pi{};
        std::strncpy(pi.disk_, disk.c_str(),  sizeof(pi.disk_) - 1);
        std::strncpy(pi.name_, name.c_str(),  sizeof(pi.name_) - 1);
        std::strncpy(pi.fs_type_, type.c_str(), sizeof(pi.fs_type_) - 1);
        pi.size_bytes_ = human.empty() ? size_sec * 512 : parseHumanSize(human);

        // Check mount
        auto it = mounts.find(name);
        if (it != mounts.end()) {
            pi.is_mounted_ = 1;
            std::strncpy(pi.mount_point_, it->second.c_str(),
                         sizeof(pi.mount_point_) - 1);
        }
        parts.push_back(pi);
    }
    return parts;
}

// ── ZFS pool enumeration ──────────────────────────────────────────────────────

std::vector<ZpoolInfo> DiskService::listZpools() noexcept {
    std::vector<ZpoolInfo> pools;

    // Bounded so a wedged/suspended pool can't hang the event loop.
    std::istringstream zss(xserver::runCapped("zpool list -H -o name,size,alloc,free,health 2>/dev/null"));
    std::string line;
    while (std::getline(zss, line)) {
        std::istringstream iss(line);
        std::string name, size_s, alloc_s, free_s, health;
        if (!(iss >> name >> size_s >> alloc_s >> free_s >> health)) continue;

        ZpoolInfo pi{};
        std::strncpy(pi.name_,   name.c_str(),   sizeof(pi.name_)   - 1);
        std::strncpy(pi.health_, health.c_str(), sizeof(pi.health_) - 1);
        pi.size_bytes_ = parseHumanSize(size_s);
        pi.used_bytes_ = parseHumanSize(alloc_s);
        pi.free_bytes_ = parseHumanSize(free_s);
        pools.push_back(pi);
    }
    return pools;
}

// ── Response helper ───────────────────────────────────────────────────────────

void DiskService::sendStatus(std::shared_ptr<IpcSocket> client,
                             bool ok, const std::string& msg_text) noexcept {
    DiskStatusResponse resp{};
    resp.success_ = ok ? 1 : 0;
    std::strncpy(resp.message_, msg_text.c_str(), sizeof(resp.message_) - 1);
    XBusMessage out{};
    std::strncpy(out.service_name_, "com.freebsdx.Disk",
                 sizeof(out.service_name_) - 1);
    out.opcode_ = static_cast<uint32_t>(DiskOpcode::DiskChanged);
    std::memcpy(out.payload_, &resp, sizeof(resp));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&out), sizeof(out)});
}

static void broadcastDiskChanged() noexcept {
    XBusMessage msg{};
    std::strncpy(msg.service_name_, "com.freebsdx.Disk",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(DiskOpcode::DiskChanged);
    BroadcastXBusMessage(msg);
}

// ── Public API ────────────────────────────────────────────────────────────────

void DiskService::init() noexcept {
    std::cout << "[DiskService] ready\n";
}

void DiskService::handleMessage(const XBusMessage& msg,
                                std::shared_ptr<IpcSocket> client) noexcept {
    auto op = static_cast<DiskOpcode>(msg.opcode_);

    // ── ListDisks ─────────────────────────────────────────────────────────────
    if (op == DiskOpcode::ListDisks) {
        auto disks = enumerateDisks();
        uint32_t total = static_cast<uint32_t>(disks.size());
        if (total == 0) {
            // Empty sentinel
            DiskListPage page{};
            XBusMessage resp{};
            std::strncpy(resp.service_name_, "com.freebsdx.Disk",
                         sizeof(resp.service_name_) - 1);
            resp.opcode_ = static_cast<uint32_t>(DiskOpcode::ListDisks);
            std::memcpy(resp.payload_, &page, sizeof(page));
            client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
            return;
        }
        for (uint32_t i = 0; i < total; ++i) {
            DiskListPage page{};
            page.page_     = i;
            page.has_more_ = (i + 1 < total) ? 1 : 0;
            page.total_    = total;
            page.count_    = 1;
            page.disk_     = disks[i];
            XBusMessage resp{};
            std::strncpy(resp.service_name_, "com.freebsdx.Disk",
                         sizeof(resp.service_name_) - 1);
            resp.opcode_ = static_cast<uint32_t>(DiskOpcode::ListDisks);
            std::memcpy(resp.payload_, &page, sizeof(page));
            client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        }
        return;
    }

    // ── ListPartitions ────────────────────────────────────────────────────────
    if (op == DiskOpcode::ListPartitions) {
        const auto* req = reinterpret_cast<const DiskNameRequest*>(msg.payload_);
        std::string disk(req->name_, strnlen(req->name_, sizeof(req->name_)));
        if (!isValidDevice(disk)) { sendStatus(client, false, "Invalid device name"); return; }

        auto parts = listPartitions(disk);
        uint32_t total = static_cast<uint32_t>(parts.size());
        if (total == 0) {
            PartitionListPage page{};
            XBusMessage resp{};
            std::strncpy(resp.service_name_, "com.freebsdx.Disk",
                         sizeof(resp.service_name_) - 1);
            resp.opcode_ = static_cast<uint32_t>(DiskOpcode::ListPartitions);
            std::memcpy(resp.payload_, &page, sizeof(page));
            client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
            return;
        }
        for (uint32_t i = 0; i < total; ++i) {
            PartitionListPage page{};
            page.page_      = i;
            page.has_more_  = (i + 1 < total) ? 1 : 0;
            page.total_     = total;
            page.count_     = 1;
            page.partition_ = parts[i];
            XBusMessage resp{};
            std::strncpy(resp.service_name_, "com.freebsdx.Disk",
                         sizeof(resp.service_name_) - 1);
            resp.opcode_ = static_cast<uint32_t>(DiskOpcode::ListPartitions);
            std::memcpy(resp.payload_, &page, sizeof(page));
            client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        }
        return;
    }

    // ── ListZpools ────────────────────────────────────────────────────────────
    if (op == DiskOpcode::ListZpools) {
        auto pools = listZpools();
        uint32_t total = static_cast<uint32_t>(pools.size());
        if (total == 0) {
            ZpoolListPage page{};
            XBusMessage resp{};
            std::strncpy(resp.service_name_, "com.freebsdx.Disk",
                         sizeof(resp.service_name_) - 1);
            resp.opcode_ = static_cast<uint32_t>(DiskOpcode::ListZpools);
            std::memcpy(resp.payload_, &page, sizeof(page));
            client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
            return;
        }
        for (uint32_t i = 0; i < total; ++i) {
            ZpoolListPage page{};
            page.page_     = i;
            page.has_more_ = (i + 1 < total) ? 1 : 0;
            page.total_    = total;
            page.count_    = 1;
            page.pool_     = pools[i];
            XBusMessage resp{};
            std::strncpy(resp.service_name_, "com.freebsdx.Disk",
                         sizeof(resp.service_name_) - 1);
            resp.opcode_ = static_cast<uint32_t>(DiskOpcode::ListZpools);
            std::memcpy(resp.payload_, &page, sizeof(page));
            client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        }
        return;
    }

    // ── Mount ─────────────────────────────────────────────────────────────────
    if (op == DiskOpcode::Mount) {
        const auto* req = reinterpret_cast<const DiskMountRequest*>(msg.payload_);
        std::string dev(req->device_,      strnlen(req->device_,      sizeof(req->device_)));
        std::string mnt(req->mount_point_, strnlen(req->mount_point_, sizeof(req->mount_point_)));
        if (!isValidDevice(dev) || !isValidPath(mnt)) {
            sendStatus(client, false, "Invalid device or mount point"); return;
        }
        std::thread([dev, mnt, client]() {
            try {
                std::string cmd = "mount /dev/" + dev + " " + mnt + " 2>&1";
                int rc = -1;
                std::string out = fbx::xserver::spawnCaptureStatus(cmd, &rc);  // posix_spawn, fork-safe
                broadcastDiskChanged();
                DiskService::sendStatus(client, rc == 0, rc == 0 ? "Mounted" : out);
            } catch (...) {
                DiskService::sendStatus(client, false, "Internal error");
            }
        }).detach();
        return;
    }

    // ── Unmount ───────────────────────────────────────────────────────────────
    if (op == DiskOpcode::Unmount) {
        const auto* req = reinterpret_cast<const DiskNameRequest*>(msg.payload_);
        std::string dev(req->name_, strnlen(req->name_, sizeof(req->name_)));
        if (!isValidDevice(dev)) { sendStatus(client, false, "Invalid device"); return; }
        std::thread([dev, client]() {
            try {
                std::string cmd = "umount /dev/" + dev + " 2>&1";
                int rc = -1;
                std::string out = fbx::xserver::spawnCaptureStatus(cmd, &rc);  // posix_spawn, fork-safe
                broadcastDiskChanged();
                DiskService::sendStatus(client, rc == 0, rc == 0 ? "Unmounted" : out);
            } catch (...) {
                DiskService::sendStatus(client, false, "Internal error");
            }
        }).detach();
        return;
    }

    // ── Format ────────────────────────────────────────────────────────────────
    if (op == DiskOpcode::Format) {
        const auto* req = reinterpret_cast<const DiskFormatRequest*>(msg.payload_);
        std::string dev(req->device_, strnlen(req->device_, sizeof(req->device_)));
        std::string fs (req->fs_type_,strnlen(req->fs_type_,sizeof(req->fs_type_)));
        std::string lbl(req->label_,  strnlen(req->label_,  sizeof(req->label_)));

        if (!isValidDevice(dev)) { sendStatus(client, false, "Invalid device"); return; }

        // Build format command based on fs type
        std::string cmd;
        if (fs == "ufs") {
            cmd = "newfs";
            if (!lbl.empty()) cmd += " -L " + xserver::shellQuote(lbl);
            cmd += " /dev/" + dev + " 2>&1";
        } else if (fs == "msdos" || fs == "fat32") {
            cmd = "newfs_msdos -F 32";
            if (!lbl.empty()) cmd += " -L " + xserver::shellQuote(lbl);
            cmd += " /dev/" + dev + " 2>&1";
        } else {
            sendStatus(client, false, "Unsupported filesystem: " + fs);
            return;
        }

        std::thread([cmd, client]() {
            try {
                int rc = -1;
                std::string out = fbx::xserver::spawnCaptureStatus(cmd, &rc);  // posix_spawn, fork-safe
                broadcastDiskChanged();
                DiskService::sendStatus(client, rc == 0, rc == 0 ? "Format complete" : out);
            } catch (...) {
                DiskService::sendStatus(client, false, "Internal error");
            }
        }).detach();
        return;
    }

    // ── Eject ─────────────────────────────────────────────────────────────────
    if (op == DiskOpcode::Eject) {
        const auto* req = reinterpret_cast<const DiskNameRequest*>(msg.payload_);
        std::string dev(req->name_, strnlen(req->name_, sizeof(req->name_)));
        if (!isValidDevice(dev)) { sendStatus(client, false, "Invalid device"); return; }
        std::thread([dev, client]() {
            try {
                std::string cmd = "camcontrol eject /dev/" + dev + " 2>&1";
                int rc = -1;
                std::string out = fbx::xserver::spawnCaptureStatus(cmd, &rc);  // posix_spawn, fork-safe
                broadcastDiskChanged();
                DiskService::sendStatus(client, rc == 0, rc == 0 ? "Ejected" : out);
            } catch (...) {
                DiskService::sendStatus(client, false, "Internal error");
            }
        }).detach();
        return;
    }
}

} // namespace fbx
