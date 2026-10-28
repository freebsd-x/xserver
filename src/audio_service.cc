#include "xserver/audio_service.h"
#include "xserver/privacy_service.h"
#include <iostream>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <sys/mman.h>
#include <algorithm>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

AudioService& AudioService::getInstance() noexcept {
    static AudioService instance;
    return instance;
}

AudioService::~AudioService() noexcept {
    shutdown();
}

// ── /dev/mixer helpers ────────────────────────────────────────────────────────

int AudioService::readMixerVolume() noexcept {
    int fd = ::open("/dev/mixer", O_RDONLY);
    if (fd < 0) return master_vol_.load();
    int vol = 0;
    ::ioctl(fd, SOUND_MIXER_READ_VOLUME, &vol);
    ::close(fd);
    int level = vol & 0xFF; // left channel, 0-100
    master_vol_.store(level);
    return level;
}

void AudioService::writeMixerVolume(int level) noexcept {
    int fd = ::open("/dev/mixer", O_RDWR);
    if (fd < 0) return;
    int vol = (level << 8) | level; // both channels
    ::ioctl(fd, SOUND_MIXER_WRITE_VOLUME, &vol);
    ::close(fd);
}

// ── DSP init / shutdown ───────────────────────────────────────────────────────

bool AudioService::init() noexcept {
    if (running_) return true;

    dsp_fd_ = ::open(default_sink_path_.c_str(), O_WRONLY | O_NONBLOCK);
    if (dsp_fd_ < 0) {
        std::cerr << "[AudioService] Failed to open " << default_sink_path_
                  << ". Audio will be disabled.\n";
        return false;
    }

    int format = AFMT_S16_LE;
    ::ioctl(dsp_fd_, SNDCTL_DSP_SETFMT, &format);

    int channels = 2;
    ::ioctl(dsp_fd_, SNDCTL_DSP_CHANNELS, &channels);

    int speed = 44100;
    ::ioctl(dsp_fd_, SNDCTL_DSP_SPEED, &speed);
    dsp_sample_rate_ = static_cast<uint32_t>(speed);

    // Read current hardware volume into cache
    master_vol_.store(readMixerVolume());

    std::cout << "[AudioService] Initialized OSS on " << default_sink_path_
              << " (" << speed << "Hz, " << channels << " ch). Master vol: "
              << master_vol_.load() << "%\n";

    running_ = true;
    audio_thread_ = std::thread(&AudioService::audioThreadLoop, this);
    return true;
}

void AudioService::shutdown() noexcept {
    if (!running_) return;
    running_ = false;
    if (audio_thread_.joinable()) audio_thread_.join();
    if (dsp_fd_ >= 0) { ::close(dsp_fd_); dsp_fd_ = -1; }

    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto& [id, client] : clients_) {
        if (client->mapped_buffer_)
            ::munmap(client->mapped_buffer_, client->buffer_size_);
    }
    clients_.clear();
}

// ── Volume control ────────────────────────────────────────────────────────────

void AudioService::setMasterVolume(int level) noexcept {
    level = std::clamp(level, 0, 100);
    master_vol_.store(level);
    muted_.store(level == 0);
    writeMixerVolume(level);
    broadcastVolumeChanged();
}

void AudioService::adjustVolume(int delta) noexcept {
    int cur = muted_ ? 0 : master_vol_.load();
    setMasterVolume(cur + delta);
}

void AudioService::toggleMute() noexcept {
    bool now_muted = !muted_.load();
    muted_.store(now_muted);
    writeMixerVolume(now_muted ? 0 : master_vol_.load());
    broadcastVolumeChanged();
}

void AudioService::broadcastVolumeChanged() noexcept {
    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.Audio",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::AudioOpcode::VolumeChanged);

    shared::AudioVolumeState vs{};
    vs.volume_ = static_cast<uint32_t>(master_vol_.load());
    vs.muted_  = muted_.load() ? 1u : 0u;
    std::strncpy(vs.sink_path_, default_sink_path_.c_str(), sizeof(vs.sink_path_) - 1);
    std::memcpy(bcast.payload_, &vs, sizeof(vs));
    BroadcastXBusMessage(bcast);
}

// ── Device enumeration ────────────────────────────────────────────────────────

std::vector<shared::AudioDevice> AudioService::enumDevices(bool capture) noexcept {
    std::vector<shared::AudioDevice> devices;

    // Privacy: a global mic block hides all capture (input) devices, so no app can
    // discover a source to record from. Output devices are unaffected.
    if (capture && PrivacyService::micBlocked()) return devices;

    // Probe /dev/dsp, /dev/dsp0 .. /dev/dsp3
    int flags = capture ? O_RDONLY : O_WRONLY;
    static const char* paths[] = {
        "/dev/dsp", "/dev/dsp0", "/dev/dsp1", "/dev/dsp2", nullptr
    };
    int found = 0;
    for (int i = 0; paths[i]; ++i) {
        int fd = ::open(paths[i], flags | O_NONBLOCK);
        if (fd < 0) continue;
        ::close(fd);

        shared::AudioDevice dev{};
        std::strncpy(dev.path_, paths[i], sizeof(dev.path_) - 1);
        if (i == 0)
            std::snprintf(dev.name_, sizeof(dev.name_),
                          capture ? "Default Input" : "Default Output");
        else
            std::snprintf(dev.name_, sizeof(dev.name_),
                          capture ? "Capture Device %d" : "Audio Output %d", i);
        dev.channels_  = 2;
        dev.is_default_ = (default_sink_path_ == paths[i] ||
                           (default_sink_path_ == "/dev/dsp" && i == 0)) ? 1u : 0u;
        devices.push_back(dev);
        ++found;
    }

    // If nothing found, return a placeholder representing the default device
    if (found == 0) {
        shared::AudioDevice dev{};
        std::strncpy(dev.path_, "/dev/dsp", sizeof(dev.path_) - 1);
        std::strncpy(dev.name_, capture ? "Default Input" : "Default Output",
                     sizeof(dev.name_) - 1);
        dev.channels_   = 2;
        dev.is_default_ = 1;
        devices.push_back(dev);
    }
    return devices;
}

void AudioService::setEq(const eq::Gains& gains) noexcept {
    std::lock_guard<std::mutex> lk(eq_mutex_);
    eq_gains_ = eq::clampGains(gains);
    ++eq_version_;
}

// ── Mix loop ──────────────────────────────────────────────────────────────────

namespace {
// Open an OSS playback device the way init() opens the default one.
int openOutput(const std::string& path, uint32_t rate) noexcept {
    int fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    int format = AFMT_S16_LE, channels = 2, speed = static_cast<int>(rate);
    ::ioctl(fd, SNDCTL_DSP_SETFMT, &format);
    ::ioctl(fd, SNDCTL_DSP_CHANNELS, &channels);
    ::ioctl(fd, SNDCTL_DSP_SPEED, &speed);
    return fd;
}
}  // namespace

void AudioService::audioThreadLoop() noexcept {
    const size_t mix_frames     = 1024;
    const size_t bytes_per_frame = 2 * 2; // stereo * int16
    const size_t mix_bytes      = mix_frames * bytes_per_frame;

    // One mix (and EQ state) per output device in use; streams go to their
    // app's route, else the default sink.
    struct Output {
        int                  fd = -1;
        bool                 owned = true;    // the default fd belongs to init()
        bool                 used = false;
        eq::Equalizer        equalizer;
        std::vector<int32_t> mix;
    };
    std::map<std::string, Output> outputs;
    std::vector<int16_t> out_buffer(mix_frames * 2, 0);
    eq::Gains eq_gains{};
    uint32_t eq_seen = UINT32_MAX;
    std::string init_path;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        init_path = default_sink_path_;
    }

    while (running_) {
        bool eq_changed = false;
        if (uint32_t v = eq_version_.load(); v != eq_seen) {
            std::lock_guard<std::mutex> lk(eq_mutex_);
            eq_gains = eq_gains_;
            eq_seen = v;
            eq_changed = true;
        }
        for (auto& [path, o] : outputs) {
            if (eq_changed) o.equalizer.configure(eq_gains, static_cast<double>(dsp_sample_rate_));
            o.used = false;
            std::fill(o.mix.begin(), o.mix.end(), 0);
        }

        const float global_scale = muted_ ? 0.0f
                                          : static_cast<float>(master_vol_.load()) / 100.0f;

        // The output for `path`, opened on first use (a failed open is
        // remembered so it isn't retried every block).
        auto outputFor = [&](const std::string& path) -> Output& {
            auto [it, inserted] = outputs.try_emplace(path);
            Output& o = it->second;
            if (inserted) {
                if (path == init_path && dsp_fd_ >= 0) { o.fd = dsp_fd_; o.owned = false; }
                else o.fd = openOutput(path, dsp_sample_rate_);
                if (o.fd < 0) std::cerr << "[AudioService] Can't open " << path << "\n";
                o.mix.assign(mix_frames * 2, 0);
                o.equalizer.configure(eq_gains, static_cast<double>(dsp_sample_rate_));
            }
            return o;
        };

        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (const auto& [id, client] : clients_) {
                if (!client->mapped_buffer_) continue;
                Output* o = &outputFor(client->route_path_.empty() ? default_sink_path_ : client->route_path_);
                // A routed device that's gone (unplugged HDMI…) falls back to
                // the default sink rather than going silent.
                if (o->fd < 0 && !client->route_path_.empty()) o = &outputFor(default_sink_path_);
                if (o->fd < 0) continue;
                o->used = true;

                float stream_scale = global_scale *
                                     static_cast<float>(client->volume_) / 100.0f;

                const int16_t* src =
                    static_cast<const int16_t*>(client->mapped_buffer_);
                const size_t total_samples =
                    client->buffer_size_ / sizeof(int16_t);

                for (size_t i = 0; i < mix_frames * 2; ++i) {
                    o->mix[i] += static_cast<int32_t>(
                        src[(client->read_offset_ + i) % total_samples] * stream_scale);
                }
                client->read_offset_ =
                    (client->read_offset_ + mix_frames * 2) % total_samples;
            }
        }

        for (auto& [path, o] : outputs) {
            if (!o.used) continue;
            o.equalizer.process(o.mix.data(), mix_frames);
            for (size_t i = 0; i < mix_frames * 2; ++i)
                out_buffer[i] = static_cast<int16_t>(std::clamp(o.mix[i], -32768, 32767));
            auto written = ::write(o.fd, out_buffer.data(), mix_bytes);
            (void)written;
        }

        const uint32_t sleep_us =
            static_cast<uint32_t>(mix_frames) * 1000000u / dsp_sample_rate_;
        std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
    }

    for (auto& [path, o] : outputs)
        if (o.owned && o.fd >= 0) ::close(o.fd);
}

// ── Routing ───────────────────────────────────────────────────────────────────

std::vector<AudioService::StreamInfo> AudioService::streams() noexcept {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    std::vector<StreamInfo> v;
    for (const auto& [id, c] : clients_) v.push_back({id, c->app_name_, c->route_path_});
    std::sort(v.begin(), v.end(), [](const StreamInfo& a, const StreamInfo& b) { return a.id < b.id; });
    return v;
}

void AudioService::setAppRoute(const std::string& app, const std::string& path) noexcept {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    if (path.empty()) app_routes_.erase(app);
    else app_routes_[app] = path;
    for (auto& [id, c] : clients_)
        if (c->app_name_ == app) c->route_path_ = path;
}

std::string AudioService::defaultSinkPath() noexcept {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    return default_sink_path_;
}

// ── ProcessMessage ────────────────────────────────────────────────────────────

bool AudioService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                  const shared::XBusMessage& msg,
                                  UniqueFd& fd_opt) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.Audio") != 0) return false;

    if (!running_) {
        if (!init())
            std::cerr << "[AudioService] CRITICAL: Failed to initialize on first request.\n";
    }

    using namespace shared;
    XBusMessage response = msg;

    switch (static_cast<AudioOpcode>(msg.opcode_)) {

    case AudioOpcode::OpenStream: {
        if (!fd_opt.isValid()) {
            std::cerr << "[AudioService] OpenStream: no SHM fd provided.\n";
            break;
        }
        AudioStreamRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));

        auto ac = std::make_shared<AudioClient>();
        ac->sample_rate_  = req.sample_rate_;
        ac->channels_     = req.channels_;
        ac->bit_depth_    = req.bit_depth_;
        ac->buffer_size_  = req.buffer_size_;
        ac->shm_fd_       = std::move(fd_opt);
        ac->app_name_     = client->appId().empty() ? "Unknown" : client->appId();

        ac->mapped_buffer_ = ::mmap(nullptr, req.buffer_size_, PROT_READ,
                                    MAP_SHARED, ac->shm_fd_.get(), 0);
        if (ac->mapped_buffer_ == MAP_FAILED) {
            std::cerr << "[AudioService] OpenStream: mmap failed.\n";
            break;
        }

        std::lock_guard<std::mutex> lock(clients_mutex_);
        if (auto r = app_routes_.find(ac->app_name_); r != app_routes_.end())
            ac->route_path_ = r->second;
        uint32_t stream_id = msg.transaction_id_;
        clients_[stream_id] = ac;
        std::cout << "[AudioService] Stream " << stream_id
                  << " opened by " << ac->app_name_ << "\n";
        std::memcpy(response.payload_, &stream_id, sizeof(stream_id));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case AudioOpcode::CloseStream: {
        uint32_t stream_id = 0;
        std::memcpy(&stream_id, msg.payload_, sizeof(stream_id));
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(stream_id);
        if (it != clients_.end()) {
            ::munmap(it->second->mapped_buffer_, it->second->buffer_size_);
            clients_.erase(it);
            std::cout << "[AudioService] Stream " << stream_id << " closed.\n";
        }
        break;
    }

    case AudioOpcode::ListSinks:
    case AudioOpcode::ListSources: {
        bool capture = (static_cast<AudioOpcode>(msg.opcode_) == AudioOpcode::ListSources);
        auto devs = enumDevices(capture);
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));
        AudioDevicePage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(devs.size());
        if (page < devs.size()) {
            pg.valid_    = 1;
            pg.device_   = devs[page];
            pg.has_more_ = (page + 1 < devs.size()) ? 1u : 0u;
        }
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &pg, sizeof(pg));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case AudioOpcode::SetDefaultSink: {
        AudioDeviceRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.path_[sizeof(req.path_) - 1] = '\0';
        {
            // The mix thread reads it per block, so the switch takes effect
            // on the next one (it used to keep writing to the old device).
            std::lock_guard<std::mutex> lock(clients_mutex_);
            default_sink_path_ = std::string(req.path_, strnlen(req.path_, sizeof(req.path_)));
        }
        std::cout << "[AudioService] Default sink → " << req.path_ << "\n";
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case AudioOpcode::GetVolume: {
        AudioVolumeState vs{};
        vs.volume_ = static_cast<uint32_t>(readMixerVolume());
        vs.muted_  = muted_.load() ? 1u : 0u;
        std::strncpy(vs.sink_path_, default_sink_path_.c_str(),
                     sizeof(vs.sink_path_) - 1);
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &vs, sizeof(vs));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case AudioOpcode::SetVolume: {
        AudioVolumeRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        if (req.volume_ != 0xFFFFFFFFu)
            master_vol_.store(static_cast<int>(std::min(req.volume_, 100u)));
        if (req.muted_ == 0xFFFFFFFFu)
            toggleMute();
        else
            muted_.store(req.muted_ != 0);
        writeMixerVolume(muted_ ? 0 : master_vol_.load());
        broadcastVolumeChanged();
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case AudioOpcode::Mute: {
        toggleMute();
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case AudioOpcode::SetAppVolume: {
        AudioAppVolumeRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(req.stream_id_);
        if (it != clients_.end())
            it->second->volume_ = std::min(req.volume_, 100u);
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case AudioOpcode::ListStreams: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));

        std::vector<AudioStreamInfo> streams;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (const auto& [id, ac] : clients_) {
                AudioStreamInfo info{};
                info.stream_id_ = id;
                info.volume_    = ac->volume_;
                std::strncpy(info.app_name_,
                             ac->app_name_.empty() ? "Unknown" : ac->app_name_.c_str(),
                             sizeof(info.app_name_) - 1);
                streams.push_back(info);
            }
        }
        AudioStreamPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(streams.size());
        if (page < streams.size()) {
            pg.valid_    = 1;
            pg.stream_   = streams[page];
            pg.has_more_ = (page + 1 < streams.size()) ? 1u : 0u;
        }
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &pg, sizeof(pg));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    default:
        return false;
    }

    return true;
}

} // namespace fbx::xserver
