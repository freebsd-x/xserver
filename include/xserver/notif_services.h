#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace fbx::xserver {

// com.freebsdx.NotifSchedule — quiet hours (notif_parse.h), persisted in
// ~/.freebsdx-notifsched. Together with the DoNotDisturb toggle it decides
// whether NotificationService shows a notification now or holds it until the
// quiet time ends.
class NotifScheduleService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    // Hold `app`'s notification right now? (DND on or inside quiet hours, and
    // the app isn't an allowed call/alarm.) Empty app = "anything at all".
    [[nodiscard]] static bool holdNow(std::string_view app) noexcept;
};

// com.freebsdx.NotifSound — each app's notification sound (Default / Chime /
// Ping / Silent), persisted in ConfigService "notifsound.apps". Apps are added
// to the list the first time they post. The tones are synthesized and played on
// /dev/dsp (OSS) on a worker thread, honouring System Sounds on/off + volume.
class NotifSoundService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    // NotificationService: remember the app, then play its sound.
    static void noteApp(std::string_view app) noexcept;
    static void playFor(std::string_view app) noexcept;

    // Replace the OSS player (tests). Receives 48 kHz mono s16 PCM.
    using Player = void (*)(std::vector<int16_t> pcm);
    static void setPlayer(Player p) noexcept;
};

}  // namespace fbx::xserver
