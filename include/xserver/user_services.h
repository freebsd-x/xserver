#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// Per-user services: everything runs as the session user, no broker. Lists
// are parsed by user_parse.h / shared/user_env.h; every request gets a framed
// reply echoing transaction_id_ (mutations reply with an empty ack) and
// mutations broadcast the service's *Changed opcode. The dispatcher only
// routes these from the trusted shell connection.

// com.freebsdx.CronJobs — `crontab -l`. A disabled job is its line commented
// out; SetJobEnabled rewrites that line and reinstalls with crontab(1).
// RunNow detaches the command with daemon(8).
class CronJobsService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.SshKeys — ~/.ssh/authorized_keys (+ known_hosts count).
// RemoveKey rewrites the file atomically, mode 0600.
class SshKeysService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.Captures — ~/Pictures/Screenshots (screenshots) and
// ~/Videos/Captures (recordings), newest first. TakeScreenshot goes through
// the hook main() installs (ScreenshotService); DeleteCapture only takes plain
// names inside those folders.
class CapturesService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
    static void setScreenshotHook(void (*take)()) noexcept;
};

// com.freebsdx.ScreenCapture — the same folders as Captures, one file per page
// with its full path, size, PNG dimensions and mtime, newest first. Capture
// takes a full-screen shot through the Captures hook, now or after delay_secs_
// (poll() is called once a second); DeleteCapture only removes a regular file
// directly inside one of the two folders.
class ScreenCaptureService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
    static void poll() noexcept;
};

// com.freebsdx.EnvVars — the compositor's inherited environment (read-only)
// plus the user's overrides in ~/.config/freebsdx/environment (editable),
// which xshell applies to every app it launches.
class EnvVarsService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.StartupItems — the user's XDG autostart entries
// (~/.config/autostart/*.desktop, parsed by shared/autostart.h), which xshell
// launches at login. SetEnabled rewrites Hidden=; RemoveItem deletes the file.
// System-wide entries aren't listed (xshell doesn't launch them).
class StartupItemsService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
