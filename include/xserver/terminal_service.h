#pragma once

// TerminalService — compositor-side tmux-style PTY session manager
//
// Each session owns a PTY pair (forkpty) and a detached reader thread.
// Reader thread broadcasts TerminalOpcode::OutputChunk to all clients
// currently attached to that session.
//
// Sessions survive individual Terminal.xapp windows closing (DetachSession
// keeps the PTY running).  A new Terminal.xapp instance discovers them via
// ListSessions and reattaches with AttachSession.

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <set>
#include <mutex>
#include <atomic>
#include <thread>
#include <ctime>

namespace fbx::xserver {

class TerminalService {
public:
    // Route an incoming XBus message for "com.freebsdx.Terminal".
    // Returns true if the message was handled.
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    // Called by the XBus dispatcher when a client socket disconnects.
    // Implicitly detaches that client from every session it was attached to.
    static void OnClientDisconnect(std::shared_ptr<IpcSocket> client) noexcept;

private:
    // ── Internal session state ────────────────────────────────────────────────
    struct Session {
        uint32_t    id{0};
        std::string name;
        pid_t       pid{-1};
        int         master_fd{-1};
        uint32_t    created{0};
        uint32_t    last_active{0};
        // Clients currently attached to this session (receive OutputChunk).
        std::set<std::shared_ptr<IpcSocket>> attached_clients;
        // Reader thread stop flag — set to false to signal shutdown.
        std::shared_ptr<std::atomic<bool>> running;
    };

    static std::mutex                              sessions_mutex_;
    static std::unordered_map<uint32_t, Session>  sessions_;
    static std::atomic<uint32_t>                  next_id_;

    // ── Private helpers ───────────────────────────────────────────────────────

    // Fork a PTY session, start its reader thread, return session id (0 = fail).
    static uint32_t doCreate(const std::string& name,
                              uint16_t cols, uint16_t rows) noexcept;

    // Kill PTY, signal reader thread, erase session from map.
    static void doDestroy(uint32_t id) noexcept;

    // Broadcast SessionChanged to all clients.
    static void broadcastChanged() noexcept;

    // Per-session PTY reader loop — run in std::thread.
    static void readerLoop(uint32_t session_id,
                           int      master_fd,
                           std::shared_ptr<std::atomic<bool>> running) noexcept;
};

} // namespace fbx::xserver
