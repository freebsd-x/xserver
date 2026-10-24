#pragma once
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace fbx::xserver {

class XsessiondSession;  // xserver/xsessiond_session.h

class LoginService {
public:
    /// Called once from main.cc after compositor initialises.
    /// shell_spawner is called (once) after successful auth to launch xshell.
    static void setShellSpawner(std::function<void()> fn) noexcept;

    /// Spawn the greeter (xlogin).  Called instead of spawn_shell() at startup.
    static void spawnGreeter() noexcept;

    /// End the active desktop session: drop the seatd session (records the utmpx
    /// logout + closes the PAM session) so a fresh login can start. The caller is
    /// responsible for tearing down the desktop process group and re-spawning the
    /// greeter (see the logout path in main.cc).
    static void endSession() noexcept;

    /// True while the greeter process is alive and no session has started yet.
    static bool isGreeterActive() noexcept;

    /// The IpcSocket of the greeter (may be null if it has not registered yet).
    static std::shared_ptr<IpcSocket> greeterClient() noexcept;

    /// XBus message handler. Returns true if message was handled.
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

private:
    static void listUsers(std::shared_ptr<IpcSocket> client) noexcept;
    static bool pamAuthenticate(const char* user, const char* password) noexcept;
    static void startSession(const char* username) noexcept;

    static std::atomic<bool>            greeter_active_;
    static std::mutex                   state_mutex_;
    static std::weak_ptr<IpcSocket>     greeter_client_;
    static pid_t                        greeter_pid_;
    static std::function<void()>        shell_spawner_;
    // Best-effort xsessiond session, held for the desktop session's lifetime so
    // the daemon records the login (utmpx / PAM session). Null when the daemon
    // isn't available — login still proceeds. Closed on process exit.
    static std::unique_ptr<XsessiondSession> active_session_;
};

} // namespace fbx::xserver
