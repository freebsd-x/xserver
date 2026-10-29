#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// The last services the shell calls. Each answers with what this machine can
// actually do, instead of leaving the panel waiting:

// com.freebsdx.Hotspot — a Wi-Fi access point needs an adapter whose driver
// supports HOSTAP mode (`ifconfig wlanN list caps`); this one's (iwm) doesn't,
// and AP setup (hostapd + a hostap-mode wlan) isn't implemented yet, so the
// status is "off" and enabling is refused with the reason in the log. Serves
// both the HotspotOpcode and HotspotToggleOpcode shapes (same leading fields).
class HotspotService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.BtAudio — needs a Bluetooth controller (ubtN); none → empty
// list. With one, the devices named in /etc/bluetooth/hosts are listed (not
// connected — FreeBSD has no Bluetooth audio stack wired up here), and
// Connect/Disconnect are refused with the reason.
class BtAudioService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.FileSync — Syncthing folders, when Syncthing is set up: the
// folders in its config.xml with live state/progress from its REST API (curl,
// X-API-Key from the config). Pause/Resume patch the folder through the API.
// No Syncthing → empty list.
class FileSyncService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.LiveCaptions — needs a speech-recognition engine, and none is
// installed: the position/size preferences are kept, but captions stay off.
class LiveCaptionsService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.Cast — no Miracast/AirPlay sender here: Scan finds no
// receivers, the state is "not casting", Connect is refused with the reason.
class CastService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
