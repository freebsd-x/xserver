#include "xserver/desktop_services.h"
#include "xserver/desktop_parse.h"
#include "xserver/applimit_parse.h"
#include "xserver/app_sandbox_service.h"
#include "xserver/async_shell.h"
#include "xserver/config_service.h"
#include "xserver/user_files.h"
#include "xserver/xbus_reply.h"
#include "shared/logger.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <sys/sysctl.h>

namespace fbx::xserver {

namespace {

using namespace svc;
using userfiles::homeDir;
using userfiles::readFile;

// ── AppPerms ─────────────────────────────────────────────────────────────────

std::function<std::string()> g_apps_dir;

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

std::vector<InstalledApp> installedApps() noexcept {
    std::vector<InstalledApp> out;
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(g_apps_dir ? g_apps_dir() : "./apps", ec)) {
        if (de.path().extension() != ".xapp") continue;
        auto json = readFile((de.path() / "manifest.json").string());
        if (!json) continue;
        InstalledApp a{desktop::jsonString(*json, "identifier"), desktop::jsonString(*json, "name"), {}};
        std::string exe = desktop::jsonString(*json, "executable");
        if (!exe.empty()) a.exec = std::filesystem::absolute(de.path() / exe, ec).string();
        if (!a.id.empty() && a.id.size() < 32 && !a.name.empty()) out.push_back(std::move(a));
    }
    std::sort(out.begin(), out.end(), [](const InstalledApp& a, const InstalledApp& b) { return a.name < b.name; });
    return out;
}

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

// ── FastChargingService ──────────────────────────────────────────────────────

namespace {
shared::FastChargingState chargingState() {
    shared::FastChargingState st{};
    st.enabled_ = 1;
    int state = 0, rate = 0;
    size_t len = sizeof(int);
    // ACPI_BATT_STAT_CHARGING = 0x2; rate is in mW (-1 when unknown).
    if (::sysctlbyname("hw.acpi.battery.state", &state, &len, nullptr, 0) == 0 && (state & 0x2)) {
        len = sizeof(int);
        if (::sysctlbyname("hw.acpi.battery.rate", &rate, &len, nullptr, 0) == 0 && rate > 0)
            st.charge_rate_w_ = static_cast<uint32_t>((rate + 500) / 1000);
    }
    return st;
}
}  // namespace

void FastChargingService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::FastChargingOpcode;
    constexpr const char* kName = "com.freebsdx.FastCharging";
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState: {
        auto st = chargingState();
        reply(msg, client, kName, msg.opcode_, &st);
        break;
    }
    case O::SetState: {
        shared::FastChargingRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        if (!req.enabled_) LOG_INFO("FastChargingService: no charge-rate control on this system");
        ack(msg, client, kName);
        auto st = chargingState();
        broadcast(kName, static_cast<uint32_t>(O::StateChanged), &st);
        break;
    }
    default: break;
    }
}

// ── AudioRoutingService ──────────────────────────────────────────────────────

namespace {

constexpr const char* kRoutesKey = "audioroute.apps";
AudioRoutingService::Hooks g_route;

std::vector<audioroute::Output> outputs() {
    std::string text = g_route.sndstat ? g_route.sndstat() : readFile("/dev/sndstat").value_or("");
    return audioroute::parseSndstat(text);
}

// Index of the device a stream plays on (its route, else the default output).
uint32_t deviceIndex(const audioroute::Stream& st, const std::vector<audioroute::Output>& outs) {
    for (size_t i = 0; i < outs.size(); ++i)
        if (st.route.empty() ? outs[i].is_default : outs[i].path() == st.route) return static_cast<uint32_t>(i);
    return 0;
}

}  // namespace

void AudioRoutingService::init(Hooks hooks) noexcept {
    g_route = std::move(hooks);
    if (!g_route.route) return;
    for (const auto& [app, path] : audioroute::parseRoutes(ConfigService::get(kRoutesKey)))
        g_route.route(app, path);
}

void AudioRoutingService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::AudioRoutingOpcode;
    constexpr const char* kName = "com.freebsdx.AudioRouting";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListRoutes: {
        auto streams = g_route.streams ? g_route.streams() : std::vector<audioroute::Stream>{};
        auto outs = outputs();
        auto pg = makePage<shared::AudioRoutePage>(readPage(msg), streams,
            [&outs](shared::AudioRouteEntry& e, const audioroute::Stream& st) {
                std::strncpy(e.app_id_, std::to_string(st.id).c_str(), sizeof(e.app_id_) - 1);
                std::strncpy(e.app_name_, st.app.c_str(), sizeof(e.app_name_) - 1);
                e.device_count_ = static_cast<uint32_t>(outs.size());
                if (!outs.empty()) {
                    e.device_idx_ = deviceIndex(st, outs);
                    std::strncpy(e.device_name_, audioroute::shortLabel(outs[e.device_idx_]).c_str(),
                                 sizeof(e.device_name_) - 1);
                }
            });
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::SetRoute: {
        shared::AudioRouteSetRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.app_id_[sizeof(req.app_id_) - 1] = '\0';
        auto outs = outputs();
        bool ok = false;
        if (!outs.empty() && g_route.streams && g_route.route) {
            uint32_t id = static_cast<uint32_t>(std::strtoul(req.app_id_, nullptr, 10));
            for (const auto& st : g_route.streams()) {
                if (st.id != id || !audioroute::validApp(st.app)) continue;
                std::string path = outs[req.device_idx_ % outs.size()].path();
                g_route.route(st.app, path);
                auto routes = audioroute::parseRoutes(ConfigService::get(kRoutesKey));
                routes[st.app] = path;
                ConfigService::set(kRoutesKey, audioroute::formatRoutes(routes));
                LOG_INFO("AudioRoutingService: {} → {}", st.app, path);
                ok = true;
                break;
            }
        }
        if (!ok) LOG_WARN("AudioRoutingService: no stream '{}' to route", req.app_id_);
        ack(msg, client, kName);
        if (ok) broadcast<char>(kName, static_cast<uint32_t>(O::RoutesChanged), nullptr);
        break;
    }
    default: break;
    }
}

// ── DevOverlayService ────────────────────────────────────────────────────────

namespace {
std::function<bool()>     g_overlay_visible;
std::function<void(bool)> g_overlay_show;

shared::DevOverlayState overlayState() {
    shared::DevOverlayState st{};
    st.mode_ = g_overlay_visible && g_overlay_visible() ? 1u : 0u;
    return st;
}
}  // namespace

void DevOverlayService::init(std::function<bool()> visible, std::function<void(bool)> show) noexcept {
    g_overlay_visible = std::move(visible);
    g_overlay_show    = std::move(show);
}

void DevOverlayService::notifyChanged() noexcept {
    auto st = overlayState();
    broadcast("com.freebsdx.DevOverlay", static_cast<uint32_t>(shared::DevOverlayOpcode::StateChanged), &st);
}

void DevOverlayService::handleMessage(const shared::XBusMessage& msg,
                                      std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::DevOverlayOpcode;
    constexpr const char* kName = "com.freebsdx.DevOverlay";
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState: {
        auto st = overlayState();
        reply(msg, client, kName, msg.opcode_, &st);
        break;
    }
    case O::SetMode: {
        shared::DevOverlayRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        if (req.mode_ <= 1 && g_overlay_show) g_overlay_show(req.mode_ == 1);
        else LOG_INFO("DevOverlayService: mode {} isn't supported (FPS or Off)", req.mode_);
        ack(msg, client, kName);
        notifyChanged();
        break;
    }
    default: break;
    }
}

// ── AppTimeLimitService ──────────────────────────────────────────────────────

namespace {

constexpr const char* kLimitsKey   = "applimit.limits";     // id=minutes
constexpr const char* kUsageKey    = "applimit.usage";      // day|id=secs
constexpr const char* kNotifiedKey = "applimit.notified";   // day|id;id

std::mutex g_limit_mu;
AppTimeLimitService::Notify g_limit_notify;
std::string g_usage_day;
std::map<std::string, uint32_t> g_usage;          // today's seconds by app id
std::chrono::steady_clock::time_point g_usage_saved{};

// Load today's usage (or start fresh on a new day). Caller holds g_limit_mu.
void syncDay(const std::string& today) {
    if (g_usage_day == today) return;
    g_usage_day = today;
    g_usage = applimit::parseUsage(ConfigService::get(kUsageKey), today);
}

// An installed app by display name (case-insensitive) or identifier.
const InstalledApp* findApp(const std::vector<InstalledApp>& apps, std::string_view name) {
    auto lower = [](std::string_view v) {
        std::string o(v);
        for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return o;
    };
    std::string want = lower(name);
    for (const auto& a : apps)
        if (lower(a.name) == want || a.id == name) return &a;
    return nullptr;
}

}  // namespace

void AppTimeLimitService::init(Notify notify) noexcept {
    std::lock_guard lk(g_limit_mu);
    g_limit_notify = std::move(notify);
}

void AppTimeLimitService::tick(const std::string& app_id, uint32_t seconds) noexcept {
    if (app_id.empty() || seconds == 0) return;
    std::lock_guard lk(g_limit_mu);
    const std::string today = applimit::dayKey(std::time(nullptr));
    syncDay(today);
    uint32_t before = g_usage[app_id];
    uint32_t after  = before + seconds;
    g_usage[app_id] = after;

    auto limits = applimit::parseMap(ConfigService::get(kLimitsKey));
    bool hit = false;
    if (auto it = limits.find(app_id); it != limits.end() && applimit::crossed(before, after, it->second)) {
        auto told = applimit::parseNotified(ConfigService::get(kNotifiedKey), today);
        if (told.insert(app_id).second) {
            ConfigService::set(kNotifiedKey, applimit::formatNotified(today, told));
            std::string name = app_id;
            for (const auto& a : installedApps()) if (a.id == app_id) name = a.name;
            LOG_INFO("AppTimeLimit: {} reached its {} min limit", name, it->second);
            if (g_limit_notify)
                g_limit_notify("Time limit reached",
                               name + ": today's " + std::to_string(it->second) + "-minute limit is up.");
        }
        hit = true;
    }
    // Persist about once a minute (and right away when a limit is hit).
    auto now = std::chrono::steady_clock::now();
    if (hit || now - g_usage_saved >= std::chrono::minutes(1)) {
        ConfigService::set(kUsageKey, applimit::formatUsage(today, g_usage));
        g_usage_saved = now;
    }
}

void AppTimeLimitService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::AppTimeLimitOpcode;
    constexpr const char* kName = "com.freebsdx.AppTimeLimit";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListLimits: {
        auto limits = applimit::parseMap(ConfigService::get(kLimitsKey));
        std::vector<std::pair<std::string, uint32_t>> rows(limits.begin(), limits.end());
        auto apps = installedApps();
        uint32_t page = readPage(msg);
        shared::AppTimeLimitPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(rows.size());
        if (page < rows.size()) {
            const auto& [id, minutes] = rows[page];
            const InstalledApp* a = findApp(apps, id);
            std::string name = a ? a->name : id;
            uint32_t used;
            {
                std::lock_guard lk(g_limit_mu);
                syncDay(applimit::dayKey(std::time(nullptr)));
                used = g_usage.count(id) ? g_usage[id] : 0;
            }
            pg.valid_    = 1;
            pg.has_more_ = page + 1 < rows.size() ? 1u : 0u;
            std::strncpy(pg.app_name_, name.c_str(), sizeof(pg.app_name_) - 1);
            pg.limit_minutes_      = minutes;
            pg.used_minutes_today_ = used / 60;
        }
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::SetLimit:
    case O::RemoveLimit: {
        bool set = static_cast<O>(msg.opcode_) == O::SetLimit;
        char name_buf[64] = {};
        uint32_t minutes = 0;
        if (set) {
            shared::AppTimeLimitSetRequest req{};
            std::memcpy(&req, msg.payload_, sizeof(req));
            std::memcpy(name_buf, req.app_name_, sizeof(name_buf) - 1);
            minutes = req.limit_minutes_;
        } else {
            shared::AppTimeLimitRemoveRequest req{};
            std::memcpy(&req, msg.payload_, sizeof(req));
            std::memcpy(name_buf, req.app_name_, sizeof(name_buf) - 1);
        }
        auto apps = installedApps();
        const InstalledApp* a = findApp(apps, name_buf);
        bool ok = a && (!set || (minutes >= 1 && minutes <= 24 * 60));
        if (ok) {
            auto limits = applimit::parseMap(ConfigService::get(kLimitsKey));
            if (set) limits[a->id] = minutes;
            else ok = limits.erase(a->id) > 0;
            if (ok) ConfigService::set(kLimitsKey, applimit::formatMap(limits));
        }
        if (!ok) LOG_WARN("AppTimeLimit: can't {} limit for '{}'", set ? "set" : "remove", name_buf);
        ack(msg, client, kName);
        if (ok) broadcast<char>(kName, static_cast<uint32_t>(O::LimitsChanged), nullptr);
        break;
    }
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
