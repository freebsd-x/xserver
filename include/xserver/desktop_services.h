#pragma once

#include "shared/protocol.h"
#include "xserver/audio_eq.h"
#include "xserver/ipc_socket.h"
#include <functional>
#include <memory>
#include <string>

namespace fbx::xserver {

// com.freebsdx.AppPerms — camera / microphone / location per installed app
// (the .xapp bundles in the apps dir from init(), by manifest identifier,
// which xshell passes to the app as XAPP_ID). Camera and mic are the
// AppSandbox PermissionStore grants the dispatcher enforces for sandboxed
// apps; location is the LocationServices per-app policy.
class AppPermsService {
public:
    // Where the bundles live (AppManagerService::appsDir() in main).
    static void init(std::function<std::string()> apps_dir) noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.Presentation — while on, holds off the screensaver and idle
// lock (like a keep-awake) and holds notifications (like Do Not Disturb).
// Session-only: it's off again after a restart.
class PresentationService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
    [[nodiscard]] static bool isActive() noexcept;
};

// com.freebsdx.Backup — archive jobs from ~/.config/freebsdx/backups
// ("name<TAB>source<TAB>destination"; default: the home folder → ~/Backups).
// RunJob writes <dest>/<name>-YYYYmmdd-HHMMSS.tar.gz with bsdtar on a worker
// thread (excluding the destination when it's inside the source); status and
// last run persist in ConfigService "backup.<id>.*". Manual runs only.
class BackupService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.GameController — gamepads among the kernel's evdev devices
// (`sysctl kern.evdev.input`, readable without root), USB or Bluetooth.
// FreeBSD has no calibration step and pairing lives in the Bluetooth stack,
// so Calibrate/Forget are acknowledged and logged only.
class GameControllerService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.AudioEq — 5-band EQ (audio_eq.h) persisted in ConfigService
// "audioeq.*"; init() installs the hook (AudioService::setEq in main) and
// applies the saved settings.
class AudioEqService {
public:
    static void init(std::function<void(const eq::Gains&)> apply) noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
