#include "xserver/terminal_service.h"
#include <algorithm>
#include <iostream>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <libutil.h>   // forkpty on FreeBSD

// Forward-declared broadcast helper from xbus_dispatcher.cc
namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

using namespace shared;

// ── Static state ──────────────────────────────────────────────────────────────

std::mutex                              TerminalService::sessions_mutex_;
std::unordered_map<uint32_t,
    TerminalService::Session>           TerminalService::sessions_;
std::atomic<uint32_t>                  TerminalService::next_id_{1};

// ── Helpers ───────────────────────────────────────────────────────────────────

void TerminalService::broadcastChanged() noexcept {
    XBusMessage msg{};
    msg.magic_ = XBUS_MAGIC;
    std::strncpy(msg.service_name_, "com.freebsdx.Terminal",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_         = static_cast<uint32_t>(TerminalOpcode::SessionChanged);
    msg.transaction_id_ = 0;
    BroadcastXBusMessage(msg);
}

// PTY reader loop — detached thread per session.
// Reads from master_fd, breaks into ≤248-byte OutputChunk broadcasts,
// then sends to all currently-attached clients.
void TerminalService::readerLoop(uint32_t session_id, int master_fd,
                                  std::shared_ptr<std::atomic<bool>> running) noexcept {
    uint8_t buf[248];
    while (running->load()) {
        ssize_t n = ::read(master_fd, buf, sizeof(buf));
        if (n <= 0) break;   // PTY closed / EOF

        TerminalDataChunk chunk{};
        chunk.session_id_ = session_id;
        chunk.size_       = static_cast<uint32_t>(n);
        std::memcpy(chunk.data_, buf, static_cast<size_t>(n));

        XBusMessage msg{};
        msg.magic_ = XBUS_MAGIC;
        std::strncpy(msg.service_name_, "com.freebsdx.Terminal",
                     sizeof(msg.service_name_) - 1);
        msg.opcode_         = static_cast<uint32_t>(TerminalOpcode::OutputChunk);
        msg.transaction_id_ = 0;
        std::memcpy(msg.payload_, &chunk, sizeof(chunk));

        // Deliver to each attached client
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) break;

        it->second.last_active = static_cast<uint32_t>(std::time(nullptr));
        for (auto& client : it->second.attached_clients) {
            (void)client->sendMessage(
                {reinterpret_cast<const uint8_t*>(&msg), sizeof(msg)});
        }
    }
    // Reader exited — PTY process probably died
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        ::close(it->second.master_fd);
        it->second.master_fd = -1;
    }
}

uint32_t TerminalService::doCreate(const std::string& name,
                                    uint16_t cols, uint16_t rows) noexcept {
    if (cols == 0) cols = 80;
    if (rows == 0) rows = 24;

    struct winsize ws{};
    ws.ws_col = cols;
    ws.ws_row = rows;

    int master_fd = -1;
    pid_t pid = ::forkpty(&master_fd, nullptr, nullptr, &ws);
    if (pid < 0) {
        std::cerr << "[TerminalService] forkpty failed\n";
        return 0;
    }
    if (pid == 0) {
        // Child: exec the user's preferred shell
        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("XBUS_SOCKET", "/tmp/freebsdx.sock", 1);
        const char* shell = ::getenv("SHELL");
        if (!shell || shell[0] == '\0') shell = "/bin/sh";
        ::execl(shell, shell, nullptr);
        ::execlp("sh", "sh", nullptr);
        ::_exit(1);
    }

    uint32_t id = next_id_.fetch_add(1);
    auto running = std::make_shared<std::atomic<bool>>(true);

    Session session;
    session.id          = id;
    session.name        = name.empty()
                            ? ("session-" + std::to_string(id))
                            : name;
    session.pid         = pid;
    session.master_fd   = master_fd;
    session.created     = static_cast<uint32_t>(std::time(nullptr));
    session.last_active = session.created;
    session.running     = running;

    // Start reader thread (detached — lifetime managed via `running` flag)
    std::thread([session_id = id, master_fd, running]() mutable {
        readerLoop(session_id, master_fd, running);
    }).detach();

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.emplace(id, std::move(session));
    }

    std::cout << "[TerminalService] Created session " << id
              << " (pid=" << pid << ")\n";
    broadcastChanged();
    return id;
}

void TerminalService::doDestroy(uint32_t id) noexcept {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;

    Session& s = it->second;
    s.running->store(false);
    if (s.pid > 0)       ::kill(s.pid, SIGTERM);
    if (s.master_fd >= 0) ::close(s.master_fd);
    ::waitpid(s.pid, nullptr, WNOHANG);

    std::cout << "[TerminalService] Destroyed session " << id << "\n";
    sessions_.erase(it);
}

// ── ProcessMessage ─────────────────────────────────────────────────────────────

bool TerminalService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                      const XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.Terminal") != 0)
        return false;

    XBusMessage response = msg;
    response.transaction_id_ = msg.transaction_id_;

    switch (static_cast<TerminalOpcode>(msg.opcode_)) {

    // ── CreateSession ───────────────────────────────────────────────────────
    case TerminalOpcode::CreateSession: {
        TerminalCreateRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.name_[sizeof(req.name_) - 1] = '\0';

        uint32_t id = doCreate(req.name_, req.cols_, req.rows_);

        TerminalSessionReady ready{};
        ready.id_      = id;
        ready.success_ = id ? 1u : 0u;
        if (id) {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(id);
            if (it != sessions_.end()) {
                std::strncpy(ready.name_, it->second.name.c_str(),
                             sizeof(ready.name_) - 1);
                // Auto-attach creating client
                it->second.attached_clients.insert(client);
            }
        }
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &ready, sizeof(ready));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    // ── DestroySession ──────────────────────────────────────────────────────
    case TerminalOpcode::DestroySession: {
        TerminalSessionIdRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        doDestroy(req.session_id_);
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        broadcastChanged();
        break;
    }

    // ── ListSessions ────────────────────────────────────────────────────────
    case TerminalOpcode::ListSessions: {
        TerminalSessionIdRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t page = req.session_id_;  // page number reuses session_id field

        std::lock_guard<std::mutex> lock(sessions_mutex_);

        // Collect entries sorted by creation time (ascending)
        std::vector<const Session*> sorted;
        sorted.reserve(sessions_.size());
        for (const auto& [id, s] : sessions_) sorted.push_back(&s);
        std::sort(sorted.begin(), sorted.end(),
                  [](const Session* a, const Session* b){
                      return a->created < b->created; });

        constexpr uint32_t kPageSize = 3;
        uint32_t total    = static_cast<uint32_t>(sorted.size());
        uint32_t offset   = page * kPageSize;
        uint32_t count    = 0;

        TerminalListPage lp{};
        lp.page_     = page;
        lp.total_    = total;
        lp.has_more_ = (offset + kPageSize < total) ? 1u : 0u;

        for (uint32_t i = offset; i < total && count < kPageSize; ++i, ++count) {
            const Session* s = sorted[i];
            TerminalSessionEntry& e = lp.entries_[count];
            e.id_          = s->id;
            e.created_     = s->created;
            e.last_active_ = s->last_active;
            e.attached_    = s->attached_clients.empty() ? 0u : 1u;
            std::strncpy(e.name_, s->name.c_str(), sizeof(e.name_) - 1);
        }
        lp.count_ = count;

        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &lp, sizeof(lp));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    // ── AttachSession ───────────────────────────────────────────────────────
    case TerminalOpcode::AttachSession: {
        TerminalSessionIdRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));

        TerminalSessionReady ready{};
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(req.session_id_);
            if (it != sessions_.end()) {
                it->second.attached_clients.insert(client);
                ready.id_      = it->second.id;
                ready.success_ = 1u;
                std::strncpy(ready.name_, it->second.name.c_str(),
                             sizeof(ready.name_) - 1);
            }
        }
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &ready, sizeof(ready));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    // ── DetachSession ───────────────────────────────────────────────────────
    case TerminalOpcode::DetachSession: {
        TerminalSessionIdRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(req.session_id_);
            if (it != sessions_.end())
                it->second.attached_clients.erase(client);
        }
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    // ── RenameSession ───────────────────────────────────────────────────────
    case TerminalOpcode::RenameSession: {
        TerminalRenameRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.name_[sizeof(req.name_) - 1] = '\0';
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(req.session_id_);
            if (it != sessions_.end())
                it->second.name = req.name_;
        }
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        broadcastChanged();
        break;
    }

    // ── ResizeSession ───────────────────────────────────────────────────────
    case TerminalOpcode::ResizeSession: {
        TerminalResizeRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(req.session_id_);
            if (it != sessions_.end() && it->second.master_fd >= 0) {
                struct winsize ws{};
                ws.ws_col = req.cols_ ? req.cols_ : 80;
                ws.ws_row = req.rows_ ? req.rows_ : 24;
                ::ioctl(it->second.master_fd, TIOCSWINSZ, &ws);
                if (it->second.pid > 0)
                    ::kill(it->second.pid, SIGWINCH);
            }
        }
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    // ── SendInput ───────────────────────────────────────────────────────────
    case TerminalOpcode::SendInput: {
        TerminalDataChunk chunk{};
        std::memcpy(&chunk, msg.payload_, sizeof(chunk));
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(chunk.session_id_);
            if (it != sessions_.end() && it->second.master_fd >= 0 &&
                chunk.size_ > 0 && chunk.size_ <= sizeof(chunk.data_)) {
                (void)::write(it->second.master_fd,
                              chunk.data_,
                              static_cast<size_t>(chunk.size_));
                it->second.last_active =
                    static_cast<uint32_t>(std::time(nullptr));
            }
        }
        // No response for SendInput — fire-and-forget
        break;
    }

    default:
        std::cerr << "[TerminalService] Unknown opcode "
                  << msg.opcode_ << "\n";
        break;
    }

    return true;
}

// ── OnClientDisconnect ────────────────────────────────────────────────────────

void TerminalService::OnClientDisconnect(std::shared_ptr<IpcSocket> client) noexcept {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [id, session] : sessions_)
        session.attached_clients.erase(client);
}

} // namespace fbx::xserver
