#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include "xserver/audio_eq.h"
#include <memory>
#include <span>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbx::xserver {

class AudioService {
public:
    static AudioService& getInstance() noexcept;

    bool init() noexcept;
    void shutdown() noexcept;

    // Master volume (0-100) via /dev/mixer SOUND_MIXER_WRITE_VOL.
    void setMasterVolume(int level) noexcept;
    void adjustVolume(int delta) noexcept;  // clamp-safe ±delta
    void toggleMute() noexcept;

    // 5-band EQ (com.freebsdx.AudioEq); the mix thread picks it up on its next
    // block.
    void setEq(const eq::Gains& gains) noexcept;

    // Handles incoming XBus Audio service messages.
    bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                        const shared::XBusMessage& msg, UniqueFd& fd_opt) noexcept;

private:
    AudioService() noexcept = default;
    ~AudioService() noexcept;

    void audioThreadLoop() noexcept;

    int  readMixerVolume() noexcept;   // open/ioctl/close, returns 0-100
    void writeMixerVolume(int level) noexcept;
    void broadcastVolumeChanged() noexcept;

    std::vector<shared::AudioDevice> enumDevices(bool capture) noexcept;

    struct AudioClient {
        uint32_t    sample_rate_;
        uint32_t    channels_;
        uint32_t    bit_depth_;
        uint32_t    buffer_size_;
        UniqueFd    shm_fd_;
        void*       mapped_buffer_{nullptr};
        size_t      read_offset_{0};   // ring-buffer read position (samples)
        uint32_t    volume_{100};      // per-stream scalar 0-100
        std::string app_name_;         // app identifier (from IpcSocket::appId())
    };

    std::unordered_map<uint32_t, std::shared_ptr<AudioClient>> clients_;
    std::mutex clients_mutex_;

    int      dsp_fd_{-1};
    uint32_t dsp_sample_rate_{44100};
    std::atomic<bool> running_{false};
    std::thread audio_thread_;

    std::atomic<int>  master_vol_{80}; // 0-100, mirrors /dev/mixer
    std::atomic<bool> muted_{false};
    std::mutex            eq_mutex_;
    eq::Gains             eq_gains_{};
    std::atomic<uint32_t> eq_version_{0};
    std::string default_sink_path_{"/dev/dsp"};
};

} // namespace fbx::xserver
