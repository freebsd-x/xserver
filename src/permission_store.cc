#include "xserver/permission_store.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace fbx::xserver {

std::string PermissionStore::resolvePath(const std::string& path) const {
    if (!path.empty()) return path;
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.freebsdx/app-permissions";
}

bool PermissionStore::isAllowed(const std::string& app, Capability cap) const noexcept {
    if (cap >= kNumCaps) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = apps_.find(app);
    if (it != apps_.end() && it->second.set[cap]) return it->second.val[cap];
    return defaultFor(cap);
}

bool PermissionStore::set(const std::string& app, Capability cap, bool allowed) noexcept {
    if (cap >= kNumCaps || app.empty()) return false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        Rec& r = apps_[app];
        r.set[cap] = true;
        r.val[cap] = allowed;
    }
    save();
    return true;
}

std::vector<PermissionStore::AppPerms> PermissionStore::list() const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<AppPerms> out;
    out.reserve(apps_.size());
    for (const auto& [app, rec] : apps_) {   // std::map → sorted by app id
        AppPerms p;
        p.app = app;
        for (uint32_t c = 0; c < kNumCaps; ++c)
            p.allowed[c] = rec.set[c] ? rec.val[c]
                                      : defaultFor(static_cast<Capability>(c));
        out.push_back(std::move(p));
    }
    return out;
}

// Persistence format: one app per line — "app_id net cam mic fs gpu", each cap
// as 0 (denied), 1 (granted), or - (no explicit decision → uses the default).
void PermissionStore::load(const std::string& path) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    path_ = resolvePath(path);
    apps_.clear();
    std::ifstream f(path_);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::stringstream ss(line);
        std::string app;
        if (!(ss >> app)) continue;
        Rec r;
        for (uint32_t c = 0; c < kNumCaps; ++c) {
            std::string tok;
            if (!(ss >> tok)) break;
            if (tok == "-") continue;             // no explicit decision
            r.set[c] = true;
            r.val[c] = (tok == "1");
        }
        apps_[app] = r;
    }
}

bool PermissionStore::save() const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    if (path_.empty()) return false;
    std::error_code ec;
    fs::create_directories(fs::path(path_).parent_path(), ec);
    std::ofstream f(path_, std::ios::trunc);
    if (!f.is_open()) return false;
    f << "# FreeBSD-X App Permissions v1 — app net cam mic fs gpu (0/1/-)\n";
    for (const auto& [app, rec] : apps_) {
        f << app;
        for (uint32_t c = 0; c < kNumCaps; ++c)
            f << ' ' << (rec.set[c] ? (rec.val[c] ? '1' : '0') : '-');
        f << '\n';
    }
    return true;
}

}  // namespace fbx::xserver
