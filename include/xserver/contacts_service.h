#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fbx::xserver {

// com.freebsdx.Contacts — personal contact storage with vCard import.
// Persistence: ~/.freebsdx-contacts (pipe-separated, one entry per line).
// Thread-safe; all public methods may be called from the IPC thread.
class ContactsService {
public:
    static void init() noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

private:
    struct Contact {
        uint32_t    id;
        std::string first_name;
        std::string last_name;
        std::string email;
        std::string phone;
        std::string organization;
        std::string notes;
    };

    static bool loadFromDisk() noexcept;
    static bool saveToDisk() noexcept;

    // vCard parser: appends imported contacts to items_, returns count added.
    static uint32_t importVCard(const std::string& path) noexcept;

    static void fillEntry(shared::ContactEntry& out, const Contact& c) noexcept;
    static Contact fromEntry(const shared::ContactEntry& e) noexcept;

    static void sendEntry(IpcSocket* client, uint32_t tid, const Contact& c) noexcept;
    static void sendStatus(IpcSocket* client, uint32_t tid, bool ok, uint32_t count = 0) noexcept;
    static void broadcastChanged() noexcept;

    // Build a list page from a pre-filtered subset.
    static void sendListPage(IpcSocket* client, uint32_t tid,
                             const std::vector<const Contact*>& filtered,
                             uint32_t page) noexcept;

    static std::mutex           mutex_;
    static std::vector<Contact> items_;
    static uint32_t             next_id_;
    static std::string          contacts_path_;
};

} // namespace fbx::xserver
