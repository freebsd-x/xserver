#include "xserver/notif_services.h"
#include "xserver/notif_parse.h"
#include "xserver/dnd_service.h"
#include "xserver/config_service.h"
#include "xserver/toggle_store.h"
#include "xserver/xbus_reply.h"
#include "shared/logger.h"

#include <atomic>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>

namespace fbx::xserver {

namespace {

using namespace svc;

// ── Schedule ─────────────────────────────────────────────────────────────────

notif::Schedule loadSchedule() {
    // enabled start_min end_min allow_calls allow_alarms weekend_only
    uint32_t v[6] = {0, 22 * 60, 7 * 60, 1, 1, 0};
    toggle::loadInts("notifsched", v, 6);
    notif::Schedule s;
    s.enabled      = v[0] != 0;
    s.start_min    = v[1] % (24 * 60);
    s.end_min      = v[2] % (24 * 60);
    s.allow_calls  = v[3] != 0;
    s.allow_alarms = v[4] != 0;
    s.weekend_only = v[5] != 0;
    return s;
}

shared::NotifSchedule toWire(const notif::Schedule& s) {
    shared::NotifSchedule w{};
    w.enabled_      = s.enabled ? 1u : 0u;
    w.start_hour_   = s.start_min / 60;
    w.start_minute_ = s.start_min % 60;
    w.end_hour_     = s.end_min / 60;
    w.end_minute_   = s.end_min % 60;
    w.allow_calls_  = s.allow_calls ? 1u : 0u;
    w.allow_alarms_ = s.allow_alarms ? 1u : 0u;
    w.weekend_only_ = s.weekend_only ? 1u : 0u;
    return w;
}

// ── Sounds ───────────────────────────────────────────────────────────────────

constexpr const char* kSoundKey = "notifsound.apps";
constexpr size_t kMaxApps = 64;

std::mutex g_sound_mu;   // guards the ConfigService read-modify-write

uint32_t cfgU32(const char* k, uint32_t d) {
    try { return static_cast<uint32_t>(std::stoul(ConfigService::get(k, std::to_string(d)))); }
    catch (...) { return d; }
}

std::atomic<bool> g_playing{false};

// Default player: OSS /dev/dsp, 48 kHz mono s16, on a detached worker so the
// compositor never waits on the sound device. One tone at a time — a burst of
// notifications doesn't stack up sounds.
void ossPlay(std::vector<int16_t> pcm) {
    if (pcm.empty() || g_playing.exchange(true)) return;
    std::thread([pcm = std::move(pcm)] {
        int fd = ::open("/dev/dsp", O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            int fmt = AFMT_S16_LE, ch = 1, rate = 48000;
            if (::ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) == 0 && ::ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) == 0 &&
                ::ioctl(fd, SNDCTL_DSP_SPEED, &rate) == 0) {
                const char* p = reinterpret_cast<const char*>(pcm.data());
                size_t left = pcm.size() * sizeof(int16_t);
                while (left > 0) {
                    ssize_t n = ::write(fd, p, left);
                    if (n <= 0) break;
                    p += n;
                    left -= static_cast<size_t>(n);
                }
            }
            ::close(fd);
        }
        g_playing = false;
    }).detach();
}

NotifSoundService::Player g_player = &ossPlay;

void play(uint32_t sound) {
    if (!cfgU32("sound.enabled", 1)) return;              // System Sounds off
    auto pcm = notif::synthesize(sound, std::min(cfgU32("sound.volume_pct", 80), 100u));
    if (!pcm.empty() && g_player) g_player(std::move(pcm));
}

}  // namespace

// ── NotifScheduleService ─────────────────────────────────────────────────────

namespace { bool (*g_extra_hold)() = nullptr; }

void NotifScheduleService::setExtraHold(bool (*pred)()) noexcept { g_extra_hold = pred; }

bool NotifScheduleService::holdNow(std::string_view app) noexcept {
    notif::Schedule s = loadSchedule();
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    bool quiet = DoNotDisturbService::isActive() || notif::inQuietWindow(s, tm) ||
                 (g_extra_hold && g_extra_hold());
    return quiet && !notif::breaksThrough(s, app);
}

void NotifScheduleService::handleMessage(const shared::XBusMessage& msg,
                                         std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::NotifScheduleOpcode;
    constexpr const char* kName = "com.freebsdx.NotifSchedule";
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetSchedule: {
        auto w = toWire(loadSchedule());
        reply(msg, client, kName, msg.opcode_, &w);
        break;
    }
    case O::SetSchedule: {
        shared::NotifSchedule req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        bool valid = req.start_hour_ < 24 && req.end_hour_ < 24 &&
                     req.start_minute_ < 60 && req.end_minute_ < 60;
        if (valid) {
            uint32_t v[6] = {req.enabled_ ? 1u : 0u,
                             req.start_hour_ * 60 + req.start_minute_,
                             req.end_hour_ * 60 + req.end_minute_,
                             req.allow_calls_ ? 1u : 0u, req.allow_alarms_ ? 1u : 0u,
                             req.weekend_only_ ? 1u : 0u};
            toggle::saveInts("notifsched", v, 6);
        } else {
            LOG_WARN("NotifScheduleService: invalid time {}:{} - {}:{}",
                     req.start_hour_, req.start_minute_, req.end_hour_, req.end_minute_);
        }
        ack(msg, client, kName);
        auto w = toWire(loadSchedule());   // real state, so a bad request snaps back
        broadcast(kName, static_cast<uint32_t>(O::ScheduleChanged), &w);
        break;
    }
    default: break;
    }
}

// ── NotifSoundService ────────────────────────────────────────────────────────

void NotifSoundService::setPlayer(Player p) noexcept { g_player = p; }

void NotifSoundService::noteApp(std::string_view app) noexcept {
    if (!notif::validAppName(app)) return;
    std::lock_guard lk(g_sound_mu);
    auto m = notif::parseSoundMap(ConfigService::get(kSoundKey));
    if (m.count(std::string(app)) || m.size() >= kMaxApps) return;
    m[std::string(app)] = notif::kDefault;
    ConfigService::set(kSoundKey, notif::formatSoundMap(m));
}

void NotifSoundService::playFor(std::string_view app) noexcept {
    uint32_t sound = notif::kDefault;
    {
        std::lock_guard lk(g_sound_mu);
        auto m = notif::parseSoundMap(ConfigService::get(kSoundKey));
        if (auto it = m.find(std::string(app)); it != m.end()) sound = it->second;
    }
    play(sound);
}

void NotifSoundService::handleMessage(const shared::XBusMessage& msg,
                                      std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::NotifSoundOpcode;
    constexpr const char* kName = "com.freebsdx.NotifSound";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListSounds: {
        std::map<std::string, uint32_t> m;
        {
            std::lock_guard lk(g_sound_mu);
            m = notif::parseSoundMap(ConfigService::get(kSoundKey));
        }
        std::vector<std::pair<std::string, uint32_t>> apps(m.begin(), m.end());
        uint32_t page = readPage(msg);
        shared::NotifSoundPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(apps.size());
        if (page < apps.size()) {
            pg.valid_    = 1;
            pg.has_more_ = page + 1 < apps.size() ? 1u : 0u;
            std::strncpy(pg.app_name_, apps[page].first.c_str(), sizeof(pg.app_name_) - 1);
            pg.sound_id_ = apps[page].second;
        }
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::SetSound: {
        shared::NotifSoundRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.app_name_[sizeof(req.app_name_) - 1] = '\0';
        bool ok = notif::validAppName(req.app_name_) && req.sound_id_ <= notif::kSilent;
        if (ok) {
            std::lock_guard lk(g_sound_mu);
            auto m = notif::parseSoundMap(ConfigService::get(kSoundKey));
            ok = m.count(req.app_name_) || m.size() < kMaxApps;
            if (ok) {
                m[req.app_name_] = req.sound_id_;
                ConfigService::set(kSoundKey, notif::formatSoundMap(m));
            }
        }
        ack(msg, client, kName);
        if (ok) {
            play(req.sound_id_);   // preview the choice
            broadcast<char>(kName, static_cast<uint32_t>(O::SoundsChanged), nullptr);
        } else {
            LOG_WARN("NotifSoundService: can't set sound {} for '{}'", req.sound_id_, req.app_name_);
        }
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
