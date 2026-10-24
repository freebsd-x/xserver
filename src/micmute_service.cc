#include "xserver/micmute_service.h"
#include "xserver/toggle_store.h"
#include "shared/logger.h"
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#if __has_include(<sys/soundcard.h>)
#  include <sys/soundcard.h>
#  define FBX_HAVE_OSS 1
#endif

namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
uint32_t loadMuted() { uint32_t v[1] = {0}; toggle::loadInts("micmute", v, 1); return v[0]; }
std::function<void(bool)> g_apply;
void sendState(IpcSocket* c, uint32_t tid, shared::MicMuteOpcode op, uint32_t muted) {
    shared::MicMuteState st{}; st.muted_ = muted;
    shared::XBusMessage m{}; m.transaction_id_ = tid;
    std::strncpy(m.service_name_, "com.freebsdx.MicMute", sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(op);
    std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
}  // namespace

void MicMuteService::applyToMixer(bool muted) noexcept {
#ifdef FBX_HAVE_OSS
    static int saved_level = 80;   // level to restore on unmute
    int fd = ::open("/dev/mixer", O_RDWR);
    if (fd < 0) { LOG_WARN("MicMuteService: /dev/mixer unavailable"); return; }
    if (muted) {
        int cur = 0;
        if (::ioctl(fd, SOUND_MIXER_READ_MIC, &cur) == 0 && (cur & 0xff) > 0)
            saved_level = cur & 0xff;
        int zero = 0;
        ::ioctl(fd, SOUND_MIXER_WRITE_MIC, &zero);
    } else {
        int lvl = saved_level | (saved_level << 8);
        ::ioctl(fd, SOUND_MIXER_WRITE_MIC, &lvl);
    }
    ::close(fd);
    LOG_INFO("MicMuteService: mic {}", muted ? "muted" : "unmuted");
#else
    (void)muted;
#endif
}

void MicMuteService::init(std::function<void(bool)> apply) noexcept {
    g_apply = std::move(apply);
    if (g_apply) g_apply(loadMuted() != 0);   // apply persisted state at startup
}

void MicMuteService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::MicMuteOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetState:
        sendState(client.get(), msg.transaction_id_, O::GetState, loadMuted());
        break;
    case O::SetMuted: {
        shared::MicMuteRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t muted = req.muted_ ? 1u : 0u;
        toggle::saveInts("micmute", &muted, 1);
        if (g_apply) g_apply(muted != 0);   // gate the mic at the mixer
        sendState(client.get(), msg.transaction_id_, O::SetMuted, muted);   // ack
        // broadcast
        shared::MicMuteState st{}; st.muted_ = muted;
        shared::XBusMessage b{};
        std::strncpy(b.service_name_, "com.freebsdx.MicMute", sizeof(b.service_name_) - 1);
        b.opcode_ = static_cast<uint32_t>(O::StateChanged);
        std::memcpy(b.payload_, &st, sizeof(st));
        BroadcastXBusMessage(b);
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
