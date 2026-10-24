#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fbx::xserver {

// com.freebsdx.Notes — quick-note scratchpad with pinning, color labels, search.
// Persistence: ~/.freebsdx-notes (pipe-separated, one note per line).
// Thread-safe; all public methods may be called from the IPC thread.
class NotesService {
public:
    static void init() noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

private:
    struct Note {
        uint32_t    id;
        uint32_t    created;
        uint32_t    modified;
        bool        pinned;
        uint8_t     color;
        std::string title;
        std::string body;
    };

    static bool loadFromDisk() noexcept;
    static bool saveToDisk() noexcept;

    static void fillEntry(shared::NoteEntry& out, const Note& n) noexcept;
    static void sendEntry(IpcSocket* client, const Note& n) noexcept;
    static void sendStatus(IpcSocket* client, bool ok, uint32_t id = 0) noexcept;
    static void sendListPage(IpcSocket* client,
                             const std::vector<const Note*>& filtered,
                             uint32_t page) noexcept;
    static void broadcastChanged() noexcept;

    // Sorted view: pinned first, then by modified descending.
    static std::vector<const Note*> sortedView() noexcept;

    static std::mutex          mutex_;
    static std::vector<Note>   items_;
    static uint32_t            next_id_;
    static std::string         notes_path_;
};

} // namespace fbx::xserver
