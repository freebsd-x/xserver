// media_player_service.cc — compositor-side relay for com.freebsdx.MediaPlayer
//
// This service is a thin relay + state store:
//   • ReportState from a client  → update stored state, broadcast StateChanged
//   • Play/Pause/.../Seek/Volume → broadcast the command to all clients
//   • broadcastMediaKey()        → called from input event loop, broadcasts MediaKey

#include "xserver/media_player_service.h"

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }
using fbx::xserver::BroadcastXBusMessage;

#include <cstring>
#include <iostream>

namespace fbx {

using namespace shared;

std::mutex               MediaPlayerService::mutex_;
MediaPlayerState         MediaPlayerService::last_state_{};
bool                     MediaPlayerService::has_state_ = false;

void MediaPlayerService::init() noexcept {
    std::cout << "[MediaPlayerService] ready\n";
}

void MediaPlayerService::handleMessage(const XBusMessage& msg,
                                       std::shared_ptr<IpcSocket> /*client*/) noexcept {
    auto op = static_cast<MediaPlayerOpcode>(msg.opcode_);

    if (op == MediaPlayerOpcode::ReportState) {
        // Active player reporting its current state — store and rebroadcast.
        {
            std::lock_guard<std::mutex> lk(mutex_);
            std::memcpy(&last_state_, msg.payload_, sizeof(MediaPlayerState));
            has_state_ = true;
        }
        XBusMessage out = msg;
        out.opcode_ = static_cast<uint32_t>(MediaPlayerOpcode::StateChanged);
        BroadcastXBusMessage(out);
        return;
    }

    // All other opcodes (Play/Pause/Stop/Next/Previous/Seek/SetVolume) are
    // relayed as-is so the active player receives them.
    BroadcastXBusMessage(msg);
}

void MediaPlayerService::broadcastMediaKey(uint32_t key) noexcept {
    XBusMessage msg{};
    std::strncpy(msg.service_name_, "com.freebsdx.MediaPlayer",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(MediaPlayerOpcode::MediaKey);
    auto* req   = reinterpret_cast<MediaPlayerKeyRequest*>(msg.payload_);
    req->key_   = key;
    BroadcastXBusMessage(msg);
}

} // namespace fbx
