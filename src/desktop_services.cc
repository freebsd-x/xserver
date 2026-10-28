#include "xserver/desktop_services.h"
#include "xserver/desktop_parse.h"
#include "xserver/app_sandbox_service.h"
#include "xserver/async_shell.h"
#include "xserver/config_service.h"
#include "xserver/user_files.h"
#include "xserver/xbus_reply.h"
#include "shared/logger.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fbx::xserver {

namespace {

using namespace svc;
using userfiles::homeDir;
using userfiles::readFile;

// ── AppPerms ─────────────────────────────────────────────────────────────────

struct InstalledApp {
    std::string id, name;
};

std::function<std::string()> g_apps_dir;

std::vector<InstalledApp> installedApps() {
    std::vector<InstalledApp> out;
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(g_apps_dir ? g_apps_dir() : "./apps", ec)) {
        if (de.path().extension() != ".xapp") continue;
        auto json = readFile((de.path() / "manifest.json").string());
        if (!json) continue;
        InstalledApp a{desktop::jsonString(*json, "identifier"), desktop::jsonString(*json, "name")};
        if (!a.id.empty() && a.id.size() < 32) out.push_back(std::move(a));
    }
    std::sort(out.begin(), out.end(), [](const InstalledApp& a, const InstalledApp& b) { return a.name < b.name; });
    return out;
}

bool locationAllowed(const std::string& id) {
    return std::strtoul(ConfigService::get("location.app." + id, "0").c_str(), nullptr, 10) > 0;
}

// ── Presentation ─────────────────────────────────────────────────────────────

std::atomic<bool> g_presenting{false};

// ── Backup ───────────────────────────────────────────────────────────────────

std::mutex            g_backup_mu;
std::set<std::string> g_backup_running;   // job ids

std::vector<desktop::BackupJob> backupJobs() {
    std::string home = homeDir();
    return desktop::parseBackupJobs(readFile(home + "/.config/freebsdx/backups").value_or(""), home);
}

std::string localStamp(std::time_t t, const char* fmt) {
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

void runBackup(desktop::BackupJob job, std::string id) {
    std::thread([job = std::move(job), id] {
        std::error_code ec;
        std::filesystem::create_directories(job.destination, ec);
        std::time_t now = std::time(nullptr);
        std::string archive = job.destination + "/" + desktop::slug(job.name) + "-" +
                              localStamp(now, "%Y%m%d-%H%M%S") + ".tar.gz";
        std::vector<std::string> argv{"/usr/bin/tar", "-czf", archive, "-C", job.source};
        if (auto rel = desktop::relativeInside(job.source, job.destination); !rel.empty())
            argv.insert(argv.end(), {"--exclude", "./" + rel});
        argv.insert(argv.end(), {"--exclude", "./.cache", "."});
        LOG_INFO("BackupService: '{}' → {}", job.name, archive);
        int rc = spawnArgvStatus(argv);
        bool ok = rc == 0;
        if (!ok) {
            LOG_WARN("BackupService: '{}' failed (tar exit {})", job.name, rc);
            ::unlink(archive.c_str());
        }
        ConfigService::set("backup." + id + ".last", localStamp(now, "%Y-%m-%d %H:%M"));
        ConfigService::set("backup." + id + ".status", ok ? "0" : "2");
        {
            std::lock_guard lk(g_backup_mu);
            g_backup_running.erase(id);
        }
        broadcast<char>("com.freebsdx.Backup", static_cast<uint32_t>(shared::BackupOpcode::JobStateChanged), nullptr);
    }).detach();
}

// ── AudioEq ──────────────────────────────────────────────────────────────────

std::function<void(const eq::Gains&)> g_eq_apply;

shared::AudioEqConfig loadEq() {
    shared::AudioEqConfig c{};
    uint32_t preset = static_cast<uint32_t>(std::strtoul(ConfigService::get("audioeq.preset", "0").c_str(), nullptr, 10));
    c.preset_ = static_cast<uint8_t>(preset > eq::kCustom ? eq::kFlat : preset);
    eq::Gains g = eq::presetGains(c.preset_);
    if (c.preset_ == eq::kCustom) {
        std::string s = ConfigService::get("audioeq.gains", "0,0,0,0,0");
        size_t start = 0;
        for (int b = 0; b < eq::kBands && start <= s.size(); ++b) {
            size_t comma = s.find(',', start);
            g[b] = static_cast<int8_t>(std::atoi(s.substr(start, comma - start).c_str()));
            start = comma == std::string::npos ? s.size() + 1 : comma + 1;
        }
        g = eq::clampGains(g);
    }
    std::memcpy(c.gains_, g.data(), eq::kBands);
    return c;
}

eq::Gains gainsOf(const shared::AudioEqConfig& c) {
    eq::Gains g{};
    std::memcpy(g.data(), c.gains_, eq::kBands);
    return g;
}

}  // namespace

// ── AppPermsService ──────────────────────────────────────────────────────────

void AppPermsService::init(std::function<std::string()> apps_dir) noexcept {
    g_apps_dir = std::move(apps_dir);
}

void AppPermsService::handleMessage(const shared::XBusMessage& msg,
                                    std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::AppPermOpcode;
    constexpr const char* kName = "com.freebsdx.AppPerms";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListPerms: {
        auto apps = installedApps();
        auto pg = makePage<shared::AppPermPage>(readPage(msg), apps,
            [](shared::AppPermEntry& e, const InstalledApp& a) {
                std::strncpy(e.app_id_, a.id.c_str(), sizeof(e.app_id_) - 1);
                std::strncpy(e.name_, a.name.c_str(), sizeof(e.name_) - 1);
                e.camera_   = AppSandboxService::isAllowed(a.id, PermissionStore::Camera) ? 1u : 0u;
                e.mic_      = AppSandboxService::isAllowed(a.id, PermissionStore::Microphone) ? 1u : 0u;
                e.location_ = locationAllowed(a.id) ? 1u : 0u;
            });
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::SetPerm: {
        shared::AppPermSetRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.app_id_[sizeof(req.app_id_) - 1] = '\0';
        bool known = false;
        for (const auto& a : installedApps()) known = known || a.id == req.app_id_;
        bool ok = known && req.perm_ <= 2;
        if (ok) {
            bool grant = req.value_ != 0;
            if (req.perm_ == 0) AppSandboxService::setPermission(req.app_id_, PermissionStore::Camera, grant);
            else if (req.perm_ == 1) AppSandboxService::setPermission(req.app_id_, PermissionStore::Microphone, grant);
            else ConfigService::set(std::string("location.app.") + req.app_id_, grant ? "2" : "0");
        } else {
            LOG_WARN("AppPermsService: ignoring perm {} for '{}'", req.perm_, req.app_id_);
        }
        ack(msg, client, kName);
        if (ok) broadcast<char>(kName, static_cast<uint32_t>(O::PermsChanged), nullptr);
        break;
    }
    default: break;
    }
}

// ── PresentationService ──────────────────────────────────────────────────────

bool PresentationService::isActive() noexcept { return g_presenting.load(); }

void PresentationService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::PresentationOpcode;
    constexpr const char* kName = "com.freebsdx.Presentation";
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState: {
        shared::PresentationState st{};
        st.active_ = g_presenting ? 1u : 0u;
        reply(msg, client, kName, msg.opcode_, &st);
        break;
    }
    case O::SetActive: {
        shared::PresentationRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        g_presenting = req.active_ != 0;
        LOG_INFO("PresentationService: presentation mode {}", g_presenting ? "on" : "off");
        ack(msg, client, kName);
        shared::PresentationState st{};
        st.active_ = g_presenting ? 1u : 0u;
        broadcast(kName, static_cast<uint32_t>(O::StateChanged), &st);
        break;
    }
    default: break;
    }
}

// ── BackupService ────────────────────────────────────────────────────────────

void BackupService::handleMessage(const shared::XBusMessage& msg,
                                  std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::BackupOpcode;
    constexpr const char* kName = "com.freebsdx.Backup";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListJobs: {
        auto jobs = backupJobs();
        uint32_t page = readPage(msg);
        shared::BackupJobPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(jobs.size());
        if (page < jobs.size()) {
            const auto& j = jobs[page];
            std::string id = desktop::jobId(j);
            pg.valid_    = 1;
            pg.has_more_ = page + 1 < jobs.size() ? 1u : 0u;
            std::strncpy(pg.name_, j.name.c_str(), sizeof(pg.name_) - 1);
            std::strncpy(pg.id_, id.c_str(), sizeof(pg.id_) - 1);
            std::strncpy(pg.last_run_, ConfigService::get("backup." + id + ".last").c_str(), sizeof(pg.last_run_) - 1);
            std::strncpy(pg.next_run_, "Manual", sizeof(pg.next_run_) - 1);
            bool running;
            {
                std::lock_guard lk(g_backup_mu);
                running = g_backup_running.count(id) != 0;
            }
            pg.status_ = running ? 1u
                       : static_cast<uint32_t>(std::strtoul(ConfigService::get("backup." + id + ".status", "0").c_str(), nullptr, 10));
        }
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::RunJob: {
        shared::BackupJobRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.id_[sizeof(req.id_) - 1] = '\0';
        bool started = false;
        for (auto& j : backupJobs()) {
            if (desktop::jobId(j) != req.id_) continue;
            {
                std::lock_guard lk(g_backup_mu);
                if (g_backup_running.count(req.id_)) break;   // already running
                g_backup_running.insert(req.id_);
            }
            runBackup(std::move(j), req.id_);
            started = true;
            break;
        }
        if (!started) LOG_WARN("BackupService: not starting job '{}'", req.id_);
        ack(msg, client, kName);
        if (started) broadcast<char>(kName, static_cast<uint32_t>(O::JobStateChanged), nullptr);
        break;
    }
    default: break;
    }
}

// ── GameControllerService ────────────────────────────────────────────────────

void GameControllerService::handleMessage(const shared::XBusMessage& msg,
                                          std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::GameControllerOpcode;
    constexpr const char* kName = "com.freebsdx.GameController";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListControllers: {
        std::vector<desktop::EvdevDevice> pads;
        for (auto& d : desktop::parseEvdevSysctl(spawnCapture("sysctl kern.evdev.input 2>/dev/null")))
            if (desktop::isGameController(d)) pads.push_back(std::move(d));
        uint32_t page = readPage(msg);
        shared::GameControllerPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(pads.size());
        if (page < pads.size()) {
            const auto& d = pads[page];
            pg.valid_         = 1;
            pg.has_more_      = page + 1 < pads.size() ? 1u : 0u;
            pg.controller_id_ = desktop::controllerId(d);
            pg.battery_pct_   = 0xFFFFFFFFu;   // not exposed via evdev sysctls
            pg.connection_    = d.bustype == desktop::kBusBluetooth ? 1u : 0u;
            std::strncpy(pg.name_, d.name.c_str(), sizeof(pg.name_) - 1);
        }
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::CalibrateController:
    case O::ForgetController:
        LOG_INFO("GameControllerService: {} isn't available on FreeBSD (no calibration; pair/unpair in Bluetooth)",
                 static_cast<O>(msg.opcode_) == O::CalibrateController ? "calibrate" : "forget");
        ack(msg, client, kName);
        break;
    default: break;
    }
}

// ── AudioEqService ───────────────────────────────────────────────────────────

void AudioEqService::init(std::function<void(const eq::Gains&)> apply) noexcept {
    g_eq_apply = std::move(apply);
    if (g_eq_apply) g_eq_apply(gainsOf(loadEq()));
}

void AudioEqService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::AudioEqOpcode;
    constexpr const char* kName = "com.freebsdx.AudioEq";
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetEq: {
        auto c = loadEq();
        reply(msg, client, kName, msg.opcode_, &c);
        break;
    }
    case O::SetEq: {
        shared::AudioEqConfig req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint8_t preset = req.preset_ > eq::kCustom ? eq::kCustom : req.preset_;
        ConfigService::set("audioeq.preset", std::to_string(preset));
        if (preset == eq::kCustom) {
            eq::Gains g = eq::clampGains(gainsOf(req));
            std::string s;
            for (int b = 0; b < eq::kBands; ++b) s += (b ? "," : "") + std::to_string(g[b]);
            ConfigService::set("audioeq.gains", s);
        }
        auto c = loadEq();
        if (g_eq_apply) g_eq_apply(gainsOf(c));
        ack(msg, client, kName);
        broadcast(kName, static_cast<uint32_t>(O::EqChanged), &c);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
