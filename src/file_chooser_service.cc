#include "xserver/file_chooser_service.h"
#include "shared/protocol.h"
#include "shared/logger.h"
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/capsicum.h>
#include <sys/stat.h>

namespace fbx::xserver {

bool SendToShell(const shared::XBusMessage&);   // xbus_dispatcher.cc

// Static member definitions
std::mutex                              FileChooserService::mutex_;
std::vector<FileChooserService::PendingRequest> FileChooserService::pending_;
uint32_t                                FileChooserService::next_id_{1};

void FileChooserService::init() noexcept {
    LOG_INFO("FileChooserService: ready (com.freebsdx.FileChooser)");
}

// Open what the user picked for the app that asked. O_NONBLOCK: a FIFO must not
// hang the compositor on open — and only a regular file is handed over anyway. No
// O_TRUNC: the app replaces the contents when it has them, so a save that never
// happens can't empty the file. The rights are limited to what the job needs.
static UniqueFd openPicked(const char* path, bool save, std::string& error) noexcept {
    int fd = ::open(path, (save ? O_WRONLY | O_CREAT : O_RDWR) | O_NONBLOCK | O_CLOEXEC, 0644);
    if (fd < 0 && !save && (errno == EACCES || errno == EROFS || errno == EPERM))
        fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { error = std::strerror(errno); return {}; }
    UniqueFd owned(fd);
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { error = "Not a regular file"; return {}; }
    (void)::fcntl(fd, F_SETFL, 0);
    cap_rights_t rights;
    cap_rights_init(&rights, CAP_READ, CAP_WRITE, CAP_SEEK, CAP_FSTAT, CAP_FTRUNCATE, CAP_FSYNC, CAP_FCNTL);
    if (::cap_rights_limit(fd, &rights) != 0) { error = std::strerror(errno); return {}; }
    return owned;
}

static void sendResponse(IpcSocket& to, uint32_t tid, const shared::FileChooserResponse& resp, int fd = -1) noexcept {
    shared::XBusMessage reply{};
    reply.transaction_id_ = tid;
    std::strncpy(reply.service_name_, "com.freebsdx.FileChooser", sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(shared::FileChooserOpcode::Response);
    std::memcpy(reply.payload_, &resp, sizeof(resp));
    (void)to.sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)}, fd);
}

void FileChooserService::requestOpen(
        uint32_t opcode, uint32_t tid,
        const shared::FileChooserRequest& req,
        std::shared_ptr<IpcSocket> client) noexcept
{
    using O = shared::FileChooserOpcode;

    bool multi    = (opcode == static_cast<uint32_t>(O::OpenFiles));
    bool save     = (opcode == static_cast<uint32_t>(O::SaveFile) || opcode == static_cast<uint32_t>(O::SaveFd));
    bool wants_fd = (opcode == static_cast<uint32_t>(O::OpenFd)   || opcode == static_cast<uint32_t>(O::SaveFd));

    uint32_t id;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        id = next_id_++;
        pending_.push_back({id, std::weak_ptr<IpcSocket>(client), multi, save, wants_fd, tid});
    }

    // The request (with its assigned id) goes to the shell, not to every app.
    shared::FileChooserRequest show_req = req;
    show_req.opcode_      = static_cast<uint32_t>(O::Show);
    show_req.request_id_  = id;
    show_req.multi_select_ = multi ? 1u : 0u;
    show_req.save_mode_    = save  ? 1u : 0u;

    shared::XBusMessage show{};
    std::strncpy(show.service_name_, "com.freebsdx.FileChooser",
                 sizeof(show.service_name_) - 1);
    show.opcode_ = static_cast<uint32_t>(O::Show);
    std::memcpy(show.payload_, &show_req, sizeof(show_req));
    const bool shown = SendToShell(show);

    shared::FileChooserResponse ack{};
    ack.opcode_      = static_cast<uint32_t>(O::Response);
    ack.request_id_  = id;
    if (!shown && wants_fd) {   // nobody to ask: say so rather than wait forever
        {
            std::lock_guard<std::mutex> lk(mutex_);
            std::erase_if(pending_, [id](const PendingRequest& p) { return p.id == id; });
        }
        std::strncpy(ack.path_, "No file picker is available", sizeof(ack.path_) - 1);
        sendResponse(*client, tid, ack);
        return;
    }
    // Path requests get an immediate ack: it carries the assigned request_id so
    // the app can match the eventual Response. count_ 0, path_ empty = "pending".
    if (!wants_fd) sendResponse(*client, tid, ack);

    LOG_INFO("FileChooserService: request {} queued (save={}, multi={}, fd={})", id, save, multi, wants_fd);
}

void FileChooserService::handleRespond(
        const shared::FileChooserResponse& resp) noexcept
{
    PendingRequest req{};
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = std::find_if(pending_.begin(), pending_.end(),
                               [&](const PendingRequest& p){ return p.id == resp.request_id_; });
        if (it == pending_.end()) return;   // already answered, or never asked
        req = *it;
        pending_.erase(it);
    }
    auto client = req.requester.lock();
    if (!client) return;                    // the app has gone: open nothing

    shared::FileChooserResponse out = resp;
    out.opcode_ = static_cast<uint32_t>(shared::FileChooserOpcode::Response);
    out.path_[sizeof(out.path_) - 1] = '\0';

    if (resp.cancelled_) {
        LOG_INFO("FileChooserService: request {} cancelled", resp.request_id_);
        sendResponse(*client, req.wants_fd ? req.tid : 0, out);
        return;
    }
    if (!req.wants_fd) {
        LOG_INFO("FileChooserService: request {} resolved", resp.request_id_);
        sendResponse(*client, 0, out);
        return;
    }
    std::string error;
    UniqueFd fd = openPicked(out.path_, req.save_mode, error);
    if (!fd) {
        LOG_WARN("FileChooserService: request {}: {}", resp.request_id_, error);
        out.count_ = 0;
        std::memset(out.path_, 0, sizeof(out.path_));
        std::strncpy(out.path_, error.c_str(), sizeof(out.path_) - 1);
        sendResponse(*client, req.tid, out);
        return;
    }
    out.count_ = 1;
    sendResponse(*client, req.tid, out, fd.get());   // the kernel dups it for the app
    LOG_INFO("FileChooserService: request {} resolved with a descriptor", resp.request_id_);
}

} // namespace fbx::xserver
