#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fbx::xserver {

// com.freebsdx.Archive — archive management via libarchive.
// Formats/compressions are auto-detected on read.
// Write format is inferred from archive_path extension:
//   .zip          → ZIP
//   .tar          → tar (no compression)
//   .tar.bz2      → tar + bzip2
//   .tar.xz       → tar + xz
//   anything else → tar + gzip (.tar.gz / .tgz)
// Thread-safe; Extract and Create run on a dedicated background thread.
class ArchiveService {
public:
    static void init() noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

private:
    struct Entry {
        std::string path;
        uint32_t    uncompressed_size{0};
        uint32_t    compressed_size{0};
        bool        is_dir{false};
    };

    // In-memory entry cache keyed by archive path.
    struct CachedArchive {
        std::string          path;
        std::vector<Entry>   entries;
    };

    static bool listArchive(const std::string& path,
                            CachedArchive& out) noexcept;
    static bool extractArchive(const std::string& archive_path,
                               const std::string& dest_path,
                               uint32_t& out_count,
                               std::string& out_message) noexcept;
    static bool createArchive(const std::string& archive_path,
                              const std::string& source_path,
                              uint32_t& out_count,
                              std::string& out_message) noexcept;

    static void sendListPage(IpcSocket* client,
                             const std::vector<Entry>& entries,
                             uint32_t page) noexcept;
    static void sendStatus(IpcSocket* client,
                           bool ok,
                           uint32_t total,
                           const std::string& message) noexcept;
    static void broadcastDone(bool ok,
                              uint32_t total,
                              const std::string& message) noexcept;

    static std::mutex        mutex_;
    static CachedArchive     cache_;  // last-listed archive
};

} // namespace fbx::xserver
