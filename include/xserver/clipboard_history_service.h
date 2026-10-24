#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace fbx::xserver {

// com.freebsdx.ClipboardHistory
// Records every ClipboardService SetClipboard call into a capped ring buffer.
// Pinned entries are exempt from eviction. Persists to ~/.freebsdx-clipboard-history.
class ClipboardHistoryService {
public:
    static void init() noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    // Called by ClipboardService on every successful Set.
    static void record(const std::string& text) noexcept;

private:
    static constexpr uint32_t kMaxHistory = 30;

    struct Entry {
        uint32_t    id;
        bool        pinned;
        uint32_t    timestamp; // unix time
        std::string text;
    };

    static bool loadFromDisk() noexcept;
    static bool saveToDisk() noexcept;
    static void broadcastChanged() noexcept;
    static void sendPage(IpcSocket* client, uint32_t tid, uint32_t page) noexcept;
    static void sendStatus(IpcSocket* client, uint32_t tid, bool ok, uint32_t count = 0) noexcept;

    static std::mutex        mutex_;
    static std::deque<Entry> items_; // newest first
    static uint32_t          next_id_;
    static std::string       history_path_;
};

} // namespace fbx::xserver
