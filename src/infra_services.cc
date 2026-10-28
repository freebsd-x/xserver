#include "xserver/infra_services.h"
#include "xserver/infra_parse.h"
#include "xserver/async_shell.h"
#include "xserver/xbus_reply.h"
#include "shared/logger.h"

#include <cerrno>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace fbx::xserver {

namespace {

using namespace svc;

// Run argv on a detached worker (it may block on the network), then broadcast
// `opcode` on `service` so the shell re-polls. timeout(1) bounds a hung
// mount/umount/podman.
void runInBackground(std::vector<std::string> argv, const char* service, uint32_t opcode) {
    argv.insert(argv.begin(), {"/usr/bin/timeout", "30"});
    std::thread([argv = std::move(argv), service, opcode] {
        int rc = spawnArgvStatus(argv);
        std::string cmd;
        for (size_t i = 2; i < argv.size(); ++i) cmd += (i > 2 ? " " : "") + argv[i];
        if (rc == 0) LOG_INFO("{}: '{}' done", service, cmd);
        else         LOG_WARN("{}: '{}' failed (exit {})", service, cmd, rc);
        broadcast<char>(service, opcode, nullptr);
    }).detach();
}

void ackStatus(const shared::XBusMessage& msg, const std::shared_ptr<IpcSocket>& client,
               const char* service, uint32_t status) {
    reply(msg, client, service, msg.opcode_, &status);
}

// ── Shares ───────────────────────────────────────────────────────────────────

std::vector<infra::Share> currentShares() {
    std::ifstream f("/etc/fstab");
    std::stringstream fstab;
    fstab << f.rdbuf();
    return infra::collectShares(spawnCapture("mount -p -t nfs,smbfs 2>/dev/null"), fstab.str());
}

std::string sharePathFor(const infra::Share& s) {
    return s.protocol == infra::kNfs ? s.host + ":" + s.share : "//" + s.host + s.share;
}

bool userMountAllowed() {
    int v = 0;
    size_t len = sizeof(v);
    return ::sysctlbyname("vfs.usermount", &v, &len, nullptr, 0) == 0 && v == 1;
}

// "~/x" → $HOME/x; then it must be an existing directory the user owns (what
// vfs.usermount requires). Returns the resolved path, or "".
std::string ownedMountpoint(std::string mp) {
    if (mp.rfind("~/", 0) == 0)
        if (const char* home = std::getenv("HOME")) mp = home + mp.substr(1);
    if (mp.empty() || mp[0] != '/') return {};
    char real[PATH_MAX];
    if (!::realpath(mp.c_str(), real)) return {};
    struct stat st{};
    if (::stat(real, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid()) return {};
    return real;
}

uint32_t startMount(const shared::NetworkShareMountRequest& r) {
    std::string host(r.host_), share(r.share_), user(r.user_);
    if (r.protocol_ == infra::kSmb && !share.empty() && share[0] != '/') share = "/" + share;
    if (!infra::validHost(host) || !infra::validSharePath(share) || !infra::validUser(user) ||
        r.protocol_ > infra::kNfs)
        return EINVAL;
    if (r.protocol_ == infra::kSmb && r.password_[0]) {
        LOG_WARN("NetworkShareService: SMB passwords go in ~/.nsmbrc (mount_smbfs -N), not the form");
        return ENOTSUP;
    }
    if (!userMountAllowed()) {
        LOG_WARN("NetworkShareService: mounting as a user needs vfs.usermount=1");
        return EPERM;
    }
    std::string mp = ownedMountpoint(r.mountpoint_);
    if (mp.empty()) return ENOTDIR;
    std::vector<std::string> argv;
    if (r.protocol_ == infra::kNfs)
        argv = {"/sbin/mount_nfs", "-o", "noresvport,nosuid,soft,retrycnt=1", host + ":" + share, mp};
    else
        argv = {"/usr/sbin/mount_smbfs", "-N", "//" + (user.empty() ? "GUEST" : user) + "@" + host + share, mp};
    LOG_INFO("NetworkShareService: mounting {} on {}", argv[argv.size() - 2], mp);
    runInBackground(std::move(argv), "com.freebsdx.NetworkShare",
                    static_cast<uint32_t>(shared::NetworkShareMountOpcode::ShareChanged));
    return 0;
}

uint32_t startUnmount(uint32_t share_id) {
    for (const auto& s : currentShares()) {
        if (!s.mounted || infra::shareId(s.mountpoint) != share_id) continue;
        runInBackground({"/sbin/umount", s.mountpoint}, "com.freebsdx.NetworkShare",
                        static_cast<uint32_t>(shared::NetworkShareMountOpcode::ShareChanged));
        return 0;
    }
    return ENOENT;
}

// ── VMs / containers ─────────────────────────────────────────────────────────

std::vector<std::string> runningVms() {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator("/dev/vmm", ec)) {
        std::string n = de.path().filename().string();
        if (infra::validVmName(n)) out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool podmanInstalled() { return ::access("/usr/local/bin/podman", X_OK) == 0; }

std::vector<infra::Container> containers() {
    if (!podmanInstalled()) return {};
    return infra::parsePodmanPs(spawnCapture(
        "/usr/bin/timeout 5 /usr/local/bin/podman ps -a --format "
        "'{{.ID}}\t{{.Names}}\t{{.Image}}\t{{.State}}' 2>/dev/null"));
}

}  // namespace

// ── NetworkSharesService ─────────────────────────────────────────────────────

void NetworkSharesService::handleMessage(const shared::XBusMessage& msg,
                                         std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::NetworkShareOpcode;
    constexpr const char* kName = "com.freebsdx.NetworkShares";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListShares: {
        auto shares = currentShares();
        auto pg = makePage<shared::NetworkSharePage>(readPage(msg), shares,
            [](shared::NetworkShareEntry& e, const infra::Share& s) {
                std::string base = std::filesystem::path(s.mountpoint).filename().string();
                std::strncpy(e.name_, base.empty() ? s.mountpoint.c_str() : base.c_str(), sizeof(e.name_) - 1);
                std::strncpy(e.path_, sharePathFor(s).c_str(), sizeof(e.path_) - 1);
                std::strncpy(e.type_, s.protocol == infra::kNfs ? "NFS" : "SMB", sizeof(e.type_) - 1);
                e.mounted_ = s.mounted ? 1u : 0u;
            });
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::MountShare:
    case O::UnmountShare:
        // Not sent by the shell; mounting goes through com.freebsdx.NetworkShare.
        LOG_WARN("NetworkSharesService: use com.freebsdx.NetworkShare to mount/unmount");
        ack(msg, client, kName);
        break;
    default: break;
    }
}

// ── NetworkShareService ──────────────────────────────────────────────────────

void NetworkShareService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::NetworkShareMountOpcode;
    constexpr const char* kName = "com.freebsdx.NetworkShare";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListShares: {
        // One share per page (NetworkShareMountPage has no entries_ array).
        auto shares = currentShares();
        uint32_t page = readPage(msg);
        shared::NetworkShareMountPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(shares.size());
        if (page < shares.size()) {
            const auto& s = shares[page];
            pg.valid_    = 1;
            pg.has_more_ = page + 1 < shares.size() ? 1u : 0u;
            pg.share_id_ = infra::shareId(s.mountpoint);
            pg.protocol_ = s.protocol;
            pg.mounted_  = s.mounted ? 1u : 0u;
            std::string host = s.protocol == infra::kNfs ? s.host + ":" : "//" + s.host;
            std::strncpy(pg.host_, host.c_str(), sizeof(pg.host_) - 1);
            std::strncpy(pg.share_, s.share.c_str(), sizeof(pg.share_) - 1);
            std::strncpy(pg.mountpoint_, s.mountpoint.c_str(), sizeof(pg.mountpoint_) - 1);
        }
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::Mount: {
        shared::NetworkShareMountRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.host_[sizeof(req.host_) - 1] = '\0';
        req.share_[sizeof(req.share_) - 1] = '\0';
        req.mountpoint_[sizeof(req.mountpoint_) - 1] = '\0';
        req.user_[sizeof(req.user_) - 1] = '\0';
        req.password_[sizeof(req.password_) - 1] = '\0';
        uint32_t st = startMount(req);
        std::memset(req.password_, 0, sizeof(req.password_));
        if (st) LOG_WARN("NetworkShareService: mount refused ({})", std::strerror(static_cast<int>(st)));
        ackStatus(msg, client, kName, st);
        break;
    }
    case O::Unmount: {
        shared::NetworkShareRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t st = startUnmount(req.share_id_);
        if (st) LOG_WARN("NetworkShareService: no mounted share {:#x}", req.share_id_);
        ackStatus(msg, client, kName, st);
        break;
    }
    default: break;
    }
}

// ── VirtualMachinesService ───────────────────────────────────────────────────

void VirtualMachinesService::handleMessage(const shared::XBusMessage& msg,
                                           std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::VmOpcode;
    constexpr const char* kName = "com.freebsdx.VirtualMachines";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListVMs: {
        auto vms = runningVms();
        auto pg = makePage<shared::VmPage>(readPage(msg), vms,
            [](shared::VmEntry& e, const std::string& name) {
                std::strncpy(e.name_, name.c_str(), sizeof(e.name_) - 1);
                std::strncpy(e.type_, "bhyve", sizeof(e.type_) - 1);
                e.state_ = 1;   // listed under /dev/vmm = running
            });
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::StartVM:
    case O::StopVM:
        // Both need root and (for start) a VM definition; not wired yet.
        LOG_WARN("VirtualMachinesService: start/stop not supported yet");
        ack(msg, client, kName);
        break;
    default: break;
    }
}

// ── ContainersService ────────────────────────────────────────────────────────

void ContainersService::handleMessage(const shared::XBusMessage& msg,
                                      std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::ContainerOpcode;
    constexpr const char* kName = "com.freebsdx.Containers";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListContainers: {
        auto cs = containers();
        auto pg = makePage<shared::ContainerPage>(readPage(msg), cs,
            [](shared::ContainerEntry& e, const infra::Container& c) {
                std::strncpy(e.id_, c.id.c_str(), sizeof(e.id_) - 1);
                std::strncpy(e.name_, c.name.c_str(), sizeof(e.name_) - 1);
                std::strncpy(e.image_, c.image.c_str(), sizeof(e.image_) - 1);
                e.state_ = c.state;
            });
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::StartContainer:
    case O::StopContainer: {
        shared::ContainerActionRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.id_[sizeof(req.id_) - 1] = '\0';
        bool start = static_cast<O>(msg.opcode_) == O::StartContainer;
        if (podmanInstalled() && infra::validContainerId(req.id_))
            runInBackground({"/usr/local/bin/podman", start ? "start" : "stop", req.id_},
                            kName, static_cast<uint32_t>(O::ContainersChanged));
        else
            LOG_WARN("ContainersService: can't {} '{}'", start ? "start" : "stop", req.id_);
        ack(msg, client, kName);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
