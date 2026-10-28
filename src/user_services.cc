#include "xserver/user_services.h"
#include "xserver/user_parse.h"
#include "xserver/async_shell.h"
#include "xserver/xbus_reply.h"
#include "shared/user_env.h"
#include "shared/logger.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fbx::xserver {

namespace {

using namespace svc;

std::string homeDir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (const passwd* pw = ::getpwuid(::getuid())) return pw->pw_dir;
    return "/tmp";
}

std::optional<std::string> readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    std::stringstream body;
    body << f.rdbuf();
    return body.str();
}

// Write via a temp file in the same directory + rename, so a crash never
// leaves a half-written file behind.
bool writeFileAtomic(const std::string& path, const std::string& content, mode_t mode) {
    std::string tmpl = path + ".XXXXXX";
    int fd = ::mkstemp(tmpl.data());
    if (fd < 0) return false;
    bool ok = ::fchmod(fd, mode) == 0;
    size_t off = 0;
    while (ok && off < content.size()) {
        ssize_t n = ::write(fd, content.data() + off, content.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false; else off += static_cast<size_t>(n);
    }
    ok = (::fsync(fd) == 0) && ok;
    ::close(fd);
    if (ok && ::rename(tmpl.c_str(), path.c_str()) == 0) return true;
    ::unlink(tmpl.c_str());
    return false;
}

// posix_spawn a fixed argv (no shell) and wait; exit status or -1. Not
// fork/system: those deadlock the threaded compositor (see async_shell.h).
int spawnArgv(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    if (::posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return -1;
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// ── Cron ─────────────────────────────────────────────────────────────────────

constexpr const char* kCronName = "com.freebsdx.CronJobs";

// The user's crontab text; nullopt when they have none (crontab -l exits 1).
std::optional<std::string> readCrontab() {
    int rc = -1;
    std::string out = spawnCaptureStatus("/usr/bin/crontab -l 2>/dev/null", &rc);
    if (rc != 0) return std::nullopt;
    return out;
}

bool installCrontab(const std::string& text) {
    char path[] = "/tmp/fbx-crontab.XXXXXX";
    int fd = ::mkstemp(path);
    if (fd < 0) return false;
    bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ::close(fd);
    ok = ok && spawnArgv({"/usr/bin/crontab", path}) == 0;
    ::unlink(path);
    return ok;
}

// ── Captures ─────────────────────────────────────────────────────────────────

constexpr const char* kCapturesName = "com.freebsdx.Captures";

void (*g_take_screenshot)() = nullptr;

struct CaptureFile {
    std::string name;
    time_t      mtime   = 0;
    uint64_t    size    = 0;
    uint32_t    type    = 0;   // 0 = screenshot, 1 = recording
};

std::vector<std::string> captureDirs() {
    std::string home = homeDir();
    return {home + "/Pictures/Screenshots", home + "/Videos/Captures"};
}

std::vector<CaptureFile> listCaptures() {
    std::vector<CaptureFile> files;
    auto dirs = captureDirs();
    for (uint32_t type = 0; type < dirs.size(); ++type) {
        std::error_code ec;
        for (const auto& de : std::filesystem::directory_iterator(dirs[type], ec)) {
            std::string name = de.path().filename().string();
            struct stat st{};
            if (name.empty() || name[0] == '.' ||
                ::lstat(de.path().c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            files.push_back({name, st.st_mtime, static_cast<uint64_t>(st.st_size), type});
        }
    }
    std::sort(files.begin(), files.end(),
              [](const CaptureFile& a, const CaptureFile& b) { return a.mtime > b.mtime; });
    return files;
}

// ── Env ──────────────────────────────────────────────────────────────────────

constexpr const char* kEnvName = "com.freebsdx.EnvVars";

// xshell sets these itself for every launched app; an override would be
// silently ignored, so refuse it up front.
bool reservedEnvKey(std::string_view k) {
    return k == "WAYLAND_DISPLAY" || k == "XBUS_SOCKET";
}

std::vector<shared::userenv::Var> readUserEnv() {
    auto text = readFile(shared::userenv::filePath(homeDir()));
    return text ? shared::userenv::parse(*text) : std::vector<shared::userenv::Var>{};
}

bool writeUserEnv(const std::vector<shared::userenv::Var>& vars) {
    std::string path = shared::userenv::filePath(homeDir());
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    return writeFileAtomic(path, shared::userenv::serialize(vars), 0600);
}

struct EnvRow {
    std::string key, value;
    bool        readonly = false;
    uint32_t    id       = 0;
};

// User overrides first (editable), then the inherited environment (read-only,
// sorted), minus keys the user overrides.
std::vector<EnvRow> envRows() {
    std::vector<EnvRow> rows;
    auto user = readUserEnv();
    for (size_t i = 0; i < user.size(); ++i)
        rows.push_back({user[i].key, user[i].value, false, static_cast<uint32_t>(i + 1)});
    std::vector<EnvRow> inherited;
    for (char** e = environ; e && *e; ++e) {
        std::string_view kv(*e);
        size_t eq = kv.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        std::string_view k = kv.substr(0, eq);
        bool overridden = false;
        for (const auto& u : user) overridden = overridden || u.key == k;
        if (!overridden) inherited.push_back({std::string(k), std::string(kv.substr(eq + 1)), true, 0});
    }
    std::sort(inherited.begin(), inherited.end(),
              [](const EnvRow& a, const EnvRow& b) { return a.key < b.key; });
    for (size_t i = 0; i < inherited.size(); ++i) {
        inherited[i].id = static_cast<uint32_t>(1000 + i);
        rows.push_back(std::move(inherited[i]));
    }
    return rows;
}

}  // namespace

// ── CronJobsService ──────────────────────────────────────────────────────────

void CronJobsService::handleMessage(const shared::XBusMessage& msg,
                                    std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::CronOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListJobs: {
        auto jobs = userdata::parseCrontab(readCrontab().value_or(""));
        auto pg = makePage<shared::CronPage>(readPage(msg), jobs,
            [](shared::CronEntry& e, const userdata::CronJob& j) {
                std::strncpy(e.schedule_, j.schedule.c_str(), sizeof(e.schedule_) - 1);
                std::strncpy(e.command_, j.command.c_str(), sizeof(e.command_) - 1);
                e.job_id_  = j.line;
                e.enabled_ = j.enabled ? 1u : 0u;
            });
        reply(msg, client, kCronName, msg.opcode_, &pg);
        break;
    }
    case O::SetJobEnabled: {
        shared::CronJobToggleRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        bool ok = false;
        std::string updated;
        if (auto text = readCrontab();
            text && userdata::setCronEnabled(*text, req.job_id_, req.enabled_ != 0, updated)) {
            ok = installCrontab(updated);
            if (!ok) LOG_WARN("CronJobsService: crontab install failed");
        } else {
            LOG_WARN("CronJobsService: no job at line {}", req.job_id_);
        }
        ack(msg, client, kCronName);
        // Broadcast either way so a failed toggle snaps the switch back.
        broadcast<char>(kCronName, static_cast<uint32_t>(O::JobChanged), nullptr);
        if (ok) LOG_INFO("CronJobsService: job {} {}", req.job_id_, req.enabled_ ? "enabled" : "disabled");
        break;
    }
    case O::RunNow: {
        shared::CronRunRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        for (const auto& j : userdata::parseCrontab(readCrontab().value_or(""))) {
            if (j.line != req.job_id_) continue;
            std::string cmd = userdata::cronRunnableCommand(j.command);
            // daemon(8) detaches (and nulls stdio) so a long job never ties
            // up the compositor; the user's own uid, same as cron would.
            int rc = spawnArgv({"/usr/sbin/daemon", "-f", "/bin/sh", "-c", cmd});
            LOG_INFO("CronJobsService: run job {} '{}' ({})", j.line, cmd, rc);
            break;
        }
        ack(msg, client, kCronName);
        break;
    }
    default: break;
    }
}

// ── SshKeysService ───────────────────────────────────────────────────────────

void SshKeysService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::SshKeyOpcode;
    constexpr const char* kName = "com.freebsdx.SshKeys";
    const std::string ssh = homeDir() + "/.ssh";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListKeys: {
        auto keys = userdata::parseAuthorizedKeys(readFile(ssh + "/authorized_keys").value_or(""));
        auto pg = makePage<shared::SshKeyPage>(readPage(msg), keys,
            [](shared::SshKeyEntry& e, const userdata::SshKey& k) {
                std::strncpy(e.comment_, k.comment.c_str(), sizeof(e.comment_) - 1);
                std::strncpy(e.algo_, k.algo.c_str(), sizeof(e.algo_) - 1);
                e.key_id_ = k.line;
            });
        pg.known_hosts_count_ = userdata::countKnownHosts(readFile(ssh + "/known_hosts").value_or(""));
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::RemoveKey: {
        shared::SshKeyRemoveRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        const std::string path = ssh + "/authorized_keys";
        bool ok = false;
        std::string updated;
        if (auto text = readFile(path);
            text && userdata::removeAuthorizedKey(*text, req.key_id_, updated)) {
            ok = writeFileAtomic(path, updated, 0600);   // sshd's StrictModes wants 0600
            if (ok) LOG_INFO("SshKeysService: removed key at line {}", req.key_id_);
            else    LOG_WARN("SshKeysService: rewriting {} failed", path);
        } else {
            LOG_WARN("SshKeysService: no key at line {}", req.key_id_);
        }
        ack(msg, client, kName);
        if (ok) broadcast<char>(kName, static_cast<uint32_t>(O::KeysChanged), nullptr);
        break;
    }
    default: break;
    }
}

// ── CapturesService ──────────────────────────────────────────────────────────

void CapturesService::handleMessage(const shared::XBusMessage& msg,
                                    std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::CaptureOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListCaptures: {
        auto files = listCaptures();
        auto pg = makePage<shared::CapturePage>(readPage(msg), files,
            [](shared::CaptureEntry& e, const CaptureFile& f) {
                std::strncpy(e.filename_, f.name.c_str(), sizeof(e.filename_) - 1);
                std::tm tm{};
                localtime_r(&f.mtime, &tm);
                std::strftime(e.timestamp_, sizeof(e.timestamp_), "%Y-%m-%d %H:%M", &tm);
                e.type_    = f.type;
                e.size_kb_ = static_cast<uint32_t>((f.size + 1023) / 1024);
            });
        reply(msg, client, kCapturesName, msg.opcode_, &pg);
        break;
    }
    case O::TakeScreenshot: {
        if (g_take_screenshot) g_take_screenshot();
        else LOG_WARN("CapturesService: no screenshot backend");
        ack(msg, client, kCapturesName);
        broadcast<char>(kCapturesName, static_cast<uint32_t>(O::CapturesChanged), nullptr);
        break;
    }
    case O::DeleteCapture: {
        shared::CaptureDeleteRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.filename_[sizeof(req.filename_) - 1] = '\0';
        bool ok = false;
        if (!userdata::safeCaptureName(req.filename_)) {
            LOG_WARN("CapturesService: refusing to delete '{}'", req.filename_);
        } else {
            for (const auto& dir : captureDirs()) {
                std::string path = dir + "/" + req.filename_;
                struct stat st{};
                if (::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                    ::unlink(path.c_str()) == 0) {
                    LOG_INFO("CapturesService: deleted {}", path);
                    ok = true;
                    break;
                }
            }
            if (!ok) LOG_WARN("CapturesService: '{}' not found", req.filename_);
        }
        ack(msg, client, kCapturesName);
        if (ok) broadcast<char>(kCapturesName, static_cast<uint32_t>(O::CapturesChanged), nullptr);
        break;
    }
    default: break;
    }
}

void CapturesService::setScreenshotHook(void (*take)()) noexcept {
    g_take_screenshot = take;
}

// ── EnvVarsService ───────────────────────────────────────────────────────────

void EnvVarsService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::EnvVarOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListVars: {
        auto rows = envRows();
        auto pg = makePage<shared::EnvVarPage>(readPage(msg), rows,
            [](shared::EnvVarEntry& e, const EnvRow& r) {
                std::strncpy(e.key_, r.key.c_str(), sizeof(e.key_) - 1);
                std::strncpy(e.value_, r.value.c_str(), sizeof(e.value_) - 1);
                e.readonly_ = r.readonly ? 1u : 0u;
                e.entry_id_ = r.id;
            });
        reply(msg, client, kEnvName, msg.opcode_, &pg);
        break;
    }
    case O::SetVar: {
        shared::EnvSetRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.key_[sizeof(req.key_) - 1]     = '\0';
        req.value_[sizeof(req.value_) - 1] = '\0';
        bool ok = false;
        auto vars = readUserEnv();
        if (reservedEnvKey(req.key_))
            LOG_WARN("EnvVarsService: {} is set by the shell; not overridable", req.key_);
        else if (!shared::userenv::set(vars, req.key_, req.value_))
            LOG_WARN("EnvVarsService: invalid variable '{}'", req.key_);
        else if (!(ok = writeUserEnv(vars)))
            LOG_WARN("EnvVarsService: writing the environment file failed");
        ack(msg, client, kEnvName);
        if (ok) broadcast<char>(kEnvName, static_cast<uint32_t>(O::EnvChanged), nullptr);
        break;
    }
    case O::UnsetVar: {
        shared::EnvUnsetRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.key_[sizeof(req.key_) - 1] = '\0';
        auto vars = readUserEnv();
        // Inherited vars aren't in the file, so they can't be unset.
        bool ok = shared::userenv::unset(vars, req.key_) && writeUserEnv(vars);
        if (!ok) LOG_WARN("EnvVarsService: '{}' is not a user variable", req.key_);
        ack(msg, client, kEnvName);
        if (ok) broadcast<char>(kEnvName, static_cast<uint32_t>(O::EnvChanged), nullptr);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
