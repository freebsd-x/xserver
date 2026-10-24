#include "xserver/archive_service.h"
#include "shared/logger.h"
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

// ─── Static member definitions ────────────────────────────────────────────────
std::mutex                 ArchiveService::mutex_;
ArchiveService::CachedArchive ArchiveService::cache_;

// ─── Init ─────────────────────────────────────────────────────────────────────
void ArchiveService::init() noexcept {
    LOG_INFO("ArchiveService: ready (libarchive {})", ARCHIVE_VERSION_STRING);
}

// ─── List (read-only, builds in-memory entry cache) ──────────────────────────
bool ArchiveService::listArchive(const std::string& path,
                                 CachedArchive& out) noexcept {
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, path.c_str(), 65536) != ARCHIVE_OK) {
        archive_read_free(a);
        return false;
    }

    out.path = path;
    out.entries.clear();

    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        Entry e;
        const char* pname = archive_entry_pathname(entry);
        e.path  = pname ? pname : "";
        e.is_dir = S_ISDIR(archive_entry_filetype(entry)) != 0;

        if (archive_entry_size_is_set(entry))
            e.uncompressed_size = static_cast<uint32_t>(
                std::min<int64_t>(archive_entry_size(entry),
                                  std::numeric_limits<uint32_t>::max()));

        // libarchive doesn't expose per-entry compressed size in read mode;
        // leave compressed_size_ = 0 (displayed as "—").
        e.compressed_size = 0;

        out.entries.push_back(std::move(e));
        archive_read_data_skip(a);
    }

    archive_read_free(a);
    return true;
}

// ─── Extract ──────────────────────────────────────────────────────────────────
bool ArchiveService::extractArchive(const std::string& archive_path,
                                    const std::string& dest_path,
                                    uint32_t& out_count,
                                    std::string& out_message) noexcept {
    struct archive* ar = archive_read_new();
    archive_read_support_filter_all(ar);
    archive_read_support_format_all(ar);

    if (archive_read_open_filename(ar, archive_path.c_str(), 65536) != ARCHIVE_OK) {
        out_message = archive_error_string(ar);
        archive_read_free(ar);
        return false;
    }

    struct archive* aw = archive_write_disk_new();
    archive_write_disk_set_options(aw,
        ARCHIVE_EXTRACT_TIME   |
        ARCHIVE_EXTRACT_PERM   |
        ARCHIVE_EXTRACT_SECURE_NODOTDOT |
        ARCHIVE_EXTRACT_SECURE_SYMLINKS);
    archive_write_disk_set_standard_lookup(aw);

    struct archive_entry* entry;
    out_count = 0;

    while (archive_read_next_header(ar, &entry) == ARCHIVE_OK) {
        // Prepend dest_path to every entry
        const char* orig = archive_entry_pathname(entry);
        if (!orig) { archive_read_data_skip(ar); continue; }

        // Zip-slip guard: the entry name comes from the (untrusted) archive.
        // Reject absolute paths and any ".." path component so a malicious
        // archive can't write outside dest_path.
        {
            std::string rel = orig;
            bool unsafe = rel.empty() || rel.front() == '/';
            for (size_t i = 0; !unsafe && i <= rel.size(); ) {
                size_t slash = rel.find('/', i);
                size_t len = (slash == std::string::npos ? rel.size() : slash) - i;
                if (rel.compare(i, len, "..") == 0) unsafe = true;
                if (slash == std::string::npos) break;
                i = slash + 1;
            }
            if (unsafe) {
                std::cerr << "[ArchiveService] skipping unsafe entry: " << orig << "\n";
                archive_read_data_skip(ar);
                continue;
            }
        }

        std::string full = dest_path;
        if (!full.empty() && full.back() != '/') full += '/';
        full += orig;
        archive_entry_set_pathname(entry, full.c_str());

        if (archive_write_header(aw, entry) != ARCHIVE_OK) {
            archive_read_data_skip(ar);
            continue;
        }

        // Copy entry data in blocks
        if (!archive_entry_size_is_set(entry) || archive_entry_size(entry) > 0) {
            const void* buf;
            size_t sz;
            la_int64_t offset;
            while (archive_read_data_block(ar, &buf, &sz, &offset) == ARCHIVE_OK)
                archive_write_data_block(aw, buf, sz, offset);
        }
        archive_write_finish_entry(aw);
        ++out_count;
    }

    archive_read_free(ar);
    archive_write_free(aw);
    out_message = "Extracted " + std::to_string(out_count) + " entries";
    return true;
}

// ─── Create ───────────────────────────────────────────────────────────────────
bool ArchiveService::createArchive(const std::string& archive_path,
                                   const std::string& source_path,
                                   uint32_t& out_count,
                                   std::string& out_message) noexcept {
    struct archive* a = archive_write_new();

    // Infer format + compression from extension
    auto endswith = [&](const char* suf) {
        size_t al = archive_path.size(), sl = std::strlen(suf);
        return al >= sl &&
               archive_path.compare(al - sl, sl, suf) == 0;
    };

    if (endswith(".zip")) {
        archive_write_set_format_zip(a);
        archive_write_add_filter_none(a);
    } else if (endswith(".tar")) {
        archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_none(a);
    } else if (endswith(".tar.bz2") || endswith(".tbz2")) {
        archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_bzip2(a);
    } else if (endswith(".tar.xz") || endswith(".txz")) {
        archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_xz(a);
    } else {
        // Default: .tar.gz / .tgz
        archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_gzip(a);
    }

    if (archive_write_open_filename(a, archive_path.c_str()) != ARCHIVE_OK) {
        out_message = archive_error_string(a);
        archive_write_free(a);
        return false;
    }

    // Walk the source path using archive_read_disk
    struct archive* disk = archive_read_disk_new();
    archive_read_disk_set_standard_lookup(disk);

    int r = archive_read_disk_open(disk, source_path.c_str());
    if (r != ARCHIVE_OK) {
        out_message = archive_error_string(disk);
        archive_read_free(disk);
        archive_write_free(a);
        return false;
    }

    out_count = 0;
    struct archive_entry* entry = archive_entry_new();

    for (;;) {
        r = archive_read_next_header2(disk, entry);
        if (r == ARCHIVE_EOF) break;
        if (r != ARCHIVE_OK)  break;

        archive_read_disk_descend(disk);

        if (archive_write_header(a, entry) != ARCHIVE_OK) {
            continue;
        }

        // Copy file content (non-directories only)
        if (!S_ISDIR(archive_entry_filetype(entry)) && archive_entry_size(entry) > 0) {
            const char* srcpath = archive_entry_sourcepath(entry);
            if (srcpath) {
                int fd = ::open(srcpath, O_RDONLY);
                if (fd >= 0) {
                    char buf[65536];
                    ssize_t n;
                    while ((n = ::read(fd, buf, sizeof(buf))) > 0)
                        archive_write_data(a, buf, static_cast<size_t>(n));
                    ::close(fd);
                }
            }
        }
        archive_write_finish_entry(a);
        ++out_count;
    }

    archive_entry_free(entry);
    archive_read_close(disk);
    archive_read_free(disk);
    archive_write_close(a);
    archive_write_free(a);

    out_message = "Created archive with " + std::to_string(out_count) + " entries";
    return true;
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
void ArchiveService::sendListPage(IpcSocket* client,
                                  const std::vector<Entry>& entries,
                                  uint32_t page) noexcept {
    constexpr uint32_t kPerPage = 1;
    uint32_t offset = page * kPerPage;

    shared::XBusMessage resp{};
    std::strncpy(resp.service_name_, "com.freebsdx.Archive",
                 sizeof(resp.service_name_) - 1);
    resp.opcode_ = static_cast<uint32_t>(shared::ArchiveOpcode::List);

    shared::ArchiveListPage lp{};
    lp.page_     = page;
    lp.total_    = static_cast<uint32_t>(entries.size());

    if (offset < entries.size()) {
        const auto& e = entries[offset];
        lp.count_    = 1;
        lp.has_more_ = (offset + kPerPage < entries.size()) ? 1 : 0;
        std::strncpy(lp.entry_.path_, e.path.c_str(),
                     sizeof(lp.entry_.path_) - 1);
        lp.entry_.uncompressed_size_ = e.uncompressed_size;
        lp.entry_.compressed_size_   = e.compressed_size;
        lp.entry_.is_dir_            = e.is_dir ? 1 : 0;
    } else {
        lp.count_    = 0;
        lp.has_more_ = 0;
    }

    std::memcpy(resp.payload_, &lp, sizeof(lp));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
}

void ArchiveService::sendStatus(IpcSocket* client,
                                bool ok, uint32_t total,
                                const std::string& message) noexcept {
    shared::XBusMessage resp{};
    std::strncpy(resp.service_name_, "com.freebsdx.Archive",
                 sizeof(resp.service_name_) - 1);
    resp.opcode_ = static_cast<uint32_t>(shared::ArchiveOpcode::Extract);

    shared::ArchiveStatusResponse sr{};
    sr.success_       = ok ? 1 : 0;
    sr.total_entries_ = total;
    std::strncpy(sr.message_, message.c_str(), sizeof(sr.message_) - 1);
    std::memcpy(resp.payload_, &sr, sizeof(sr));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
}

void ArchiveService::broadcastDone(bool ok, uint32_t total,
                                   const std::string& message) noexcept {
    shared::XBusMessage bcast{};
    std::strncpy(bcast.service_name_, "com.freebsdx.Archive",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::ArchiveOpcode::OperationDone);

    shared::ArchiveStatusResponse sr{};
    sr.success_       = ok ? 1 : 0;
    sr.total_entries_ = total;
    std::strncpy(sr.message_, message.c_str(), sizeof(sr.message_) - 1);
    std::memcpy(bcast.payload_, &sr, sizeof(sr));
    BroadcastXBusMessage(bcast);
}

// ─── Message handler ──────────────────────────────────────────────────────────
void ArchiveService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using Op = shared::ArchiveOpcode;
    auto op  = static_cast<Op>(msg.opcode_);

    if (op == Op::List) {
        shared::ArchiveListRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.path_[sizeof(req.path_) - 1] = '\0';
        std::string path(req.path_, strnlen(req.path_, sizeof(req.path_)));

        std::lock_guard<std::mutex> lk(mutex_);
        // Re-list if path changed or cache empty
        if (cache_.path != path || cache_.entries.empty()) {
            CachedArchive fresh;
            if (!listArchive(path, fresh)) {
                sendStatus(client.get(), false, 0, "Cannot open archive");
                return;
            }
            cache_ = std::move(fresh);
        }
        sendListPage(client.get(), cache_.entries, req.page_);

    } else if (op == Op::Extract) {
        shared::ArchiveExtractRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.archive_path_[sizeof(req.archive_path_) - 1] = '\0';
        req.dest_path_   [sizeof(req.dest_path_)    - 1] = '\0';
        std::string apath(req.archive_path_, strnlen(req.archive_path_, sizeof(req.archive_path_)));
        std::string dpath(req.dest_path_, strnlen(req.dest_path_, sizeof(req.dest_path_)));

        // Ack immediately, do work on background thread
        sendStatus(client.get(), true, 0, "Extracting…");

        std::thread([apath, dpath]() {
            try {
                uint32_t count = 0;
                std::string msg;
                bool ok = extractArchive(apath, dpath, count, msg);
                broadcastDone(ok, count, msg);
            } catch (...) {
                broadcastDone(false, 0, "Internal error");
            }
        }).detach();

    } else if (op == Op::Create) {
        shared::ArchiveCreateRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.archive_path_[sizeof(req.archive_path_) - 1] = '\0';
        req.source_path_ [sizeof(req.source_path_)  - 1] = '\0';
        std::string apath(req.archive_path_, strnlen(req.archive_path_, sizeof(req.archive_path_)));
        std::string spath(req.source_path_, strnlen(req.source_path_, sizeof(req.source_path_)));

        sendStatus(client.get(), true, 0, "Creating archive…");

        std::thread([apath, spath]() {
            try {
                uint32_t count = 0;
                std::string msg;
                bool ok = createArchive(apath, spath, count, msg);
                broadcastDone(ok, count, msg);
            } catch (...) {
                broadcastDone(false, 0, "Internal error");
            }
        }).detach();
    }
}

} // namespace fbx::xserver
