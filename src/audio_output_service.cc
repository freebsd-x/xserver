#include "xserver/audio_output_service.h"
#include "xserver/audio_sinks.h"
#include "xserver/async_shell.h"
#include "xserver/priv_broker.h"
#include "xserver/priv_ops.h"
#include "shared/logger.h"

#include <cstring>
#include <string>
#include <vector>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
constexpr const char* kService = "com.freebsdx.AudioOutput";

std::vector<audio::AudioSink> listSinks() {
    // posix_spawn, not popen (popen deadlocks the threaded compositor).
    return audio::parseSndstat(spawnCapture("cat /dev/sndstat 2>/dev/null"));
}

shared::AudioSinkPage buildPage(const std::vector<audio::AudioSink>& sinks, uint32_t page) {
    shared::AudioSinkPage pg{};
    pg.page_  = page;
    pg.total_ = static_cast<uint32_t>(sinks.size());
    uint32_t start = page * 3, n = 0;
    for (; n < 3 && start + n < sinks.size(); ++n) {
        const audio::AudioSink& s = sinks[start + n];
        shared::AudioSinkEntry& e = pg.entries_[n];
        std::strncpy(e.name_, s.name.c_str(), sizeof(e.name_) - 1);
        std::strncpy(e.id_,   s.id.c_str(),   sizeof(e.id_) - 1);
        e.active_ = s.active ? 1u : 0u;
    }
    pg.count_    = n;
    pg.has_more_ = (start + n < sinks.size()) ? 1u : 0u;
    return pg;
}
}  // namespace

void AudioOutputService::handleMessage(const shared::XBusMessage& msg,
                                       std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::AudioOutputOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListSinks: {
        uint32_t page = 0; std::memcpy(&page, msg.payload_, sizeof(page));
        auto pg = buildPage(listSinks(), page);
        shared::XBusMessage reply{};
        reply.transaction_id_ = msg.transaction_id_;
        std::strncpy(reply.service_name_, kService, sizeof(reply.service_name_) - 1);
        reply.opcode_ = static_cast<uint32_t>(O::ListSinks);
        std::memcpy(reply.payload_, &pg, sizeof(pg));
        if (client) client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
        break;
    }
    case O::SetActiveSink: {
        shared::AudioSinkSelectRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.id_[sizeof(req.id_) - 1] = '\0';
        uint32_t unit = audio::unitOfId(req.id_);
        if (unit != UINT32_MAX) {
            // Switch the default sink via the root privilege broker; fall back to
            // a direct best-effort sysctl when the broker isn't running.
            int r = runPrivilegedOp(privops::kAudioDefaultUnit, std::to_string(unit));
            if (r == 0) {
                LOG_INFO("AudioOutputService: default sink set to {} via broker", req.id_);
            } else {
                if (r != kBrokerUnreachable)
                    LOG_WARN("AudioOutputService: broker refused default-sink set ({})", r);
                int rc = spawnStatus("sysctl hw.snd.default_unit=" + std::to_string(unit) + " >/dev/null 2>&1");
                if (rc != 0)
                    LOG_INFO("AudioOutputService: could not set default sink to {} (needs root)", req.id_);
            }
        }
        // Ack on the request's transaction, then broadcast the refreshed list.
        shared::XBusMessage ack{};
        ack.transaction_id_ = msg.transaction_id_;
        std::strncpy(ack.service_name_, kService, sizeof(ack.service_name_) - 1);
        ack.opcode_ = static_cast<uint32_t>(O::SetActiveSink);
        if (client) client->sendMessage({reinterpret_cast<const uint8_t*>(&ack), sizeof(ack)});

        shared::XBusMessage b{};
        std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
        b.opcode_ = static_cast<uint32_t>(O::SinkChanged);
        auto pg = buildPage(listSinks(), 0);
        std::memcpy(b.payload_, &pg, sizeof(pg));
        BroadcastXBusMessage(b);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
