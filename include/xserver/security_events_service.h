#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace fbx::xserver {

// com.freebsdx.Security — audit log of security-relevant events (auth failures,
// lockouts, and successful logins). Records go into a capped in-memory ring that
// is persisted to ~/.freebsdx/security-events so the audit trail survives a
// compositor restart. The login greeter and screen locker call record() on each
// authentication decision; the shell's Security panel lists them.
//   ListEvents (uint32_t page) → SecurityEventPage (3 per page, newest first)
//   ClearEvents                → wipe the log (no reply)
//   EventAdded                 → broadcast when a new event is recorded
class SecurityEventsService {
public:
    static void init() noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    // Called by the auth paths. type e.g. "login"/"unlock"/"fail"/"lockout";
    // source is an IP or "local"; success 1/0. Records, persists, broadcasts.
    static void record(const std::string& type, const std::string& user,
                       const std::string& source, bool success) noexcept;

private:
    static constexpr uint32_t kMaxEvents = 200;

    static void sendPage(IpcSocket* client, uint32_t tid, uint32_t page) noexcept;
    static bool loadFromDisk() noexcept;
    static bool saveToDisk() noexcept;

    static std::mutex                        mutex_;
    static std::deque<shared::SecurityEvent> events_;   // newest first
    static std::string                       log_path_;
};

}  // namespace fbx::xserver
