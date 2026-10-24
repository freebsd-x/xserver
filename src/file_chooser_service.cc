#include "xserver/file_chooser_service.h"
#include "shared/protocol.h"
#include "shared/logger.h"
#include <cstring>
#include <algorithm>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

// Static member definitions
std::mutex                              FileChooserService::mutex_;
std::vector<FileChooserService::PendingRequest> FileChooserService::pending_;
uint32_t                                FileChooserService::next_id_{1};

void FileChooserService::init() noexcept {
    LOG_INFO("FileChooserService: ready (com.freebsdx.FileChooser)");
}

void FileChooserService::requestOpen(
        uint32_t opcode, uint32_t tid,
        const shared::FileChooserRequest& req,
        std::shared_ptr<IpcSocket> client) noexcept
{
    using O = shared::FileChooserOpcode;

    bool multi = (opcode == static_cast<uint32_t>(O::OpenFiles));
    bool save  = (opcode == static_cast<uint32_t>(O::SaveFile));

    uint32_t id;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        id = next_id_++;
        pending_.push_back({id, std::weak_ptr<IpcSocket>(client), multi, save});
    }

    // Build Show broadcast carrying the request (with assigned request_id_)
    shared::FileChooserRequest show_req = req;
    show_req.opcode_      = static_cast<uint32_t>(O::Show);
    show_req.request_id_  = id;
    show_req.multi_select_ = multi ? 1u : 0u;
    show_req.save_mode_    = save  ? 1u : 0u;

    shared::XBusMessage bcast{};
    std::strncpy(bcast.service_name_, "com.freebsdx.FileChooser",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(O::Show);
    std::memcpy(bcast.payload_, &show_req, sizeof(show_req));
    BroadcastXBusMessage(bcast);

    // Send immediate ack to app: carries the assigned request_id so the app
    // can match the eventual Response broadcast.
    shared::FileChooserResponse ack{};
    ack.opcode_      = static_cast<uint32_t>(O::Response);
    ack.request_id_  = id;
    ack.cancelled_   = 0;
    ack.count_       = 0;   // path_[0]=='\0' signals "pending"

    shared::XBusMessage reply{};
    reply.transaction_id_ = tid;   // echo so the app can match its Call
    std::strncpy(reply.service_name_, "com.freebsdx.FileChooser",
                 sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(O::Response);
    std::memcpy(reply.payload_, &ack, sizeof(ack));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});

    LOG_INFO("FileChooserService: request {} queued (save={}, multi={})", id, save, multi);
}

void FileChooserService::handleRespond(
        const shared::FileChooserResponse& resp) noexcept
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = std::find_if(pending_.begin(), pending_.end(),
                               [&](const PendingRequest& p){ return p.id == resp.request_id_; });
        if (it != pending_.end()) pending_.erase(it);
    }

    // Broadcast result — apps match by request_id_
    shared::XBusMessage bcast{};
    std::strncpy(bcast.service_name_, "com.freebsdx.FileChooser",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::FileChooserOpcode::Response);
    std::memcpy(bcast.payload_, &resp, sizeof(resp));
    BroadcastXBusMessage(bcast);

    if (resp.cancelled_)
        LOG_INFO("FileChooserService: request {} cancelled", resp.request_id_);
    else
        LOG_INFO("FileChooserService: request {} resolved → {}", resp.request_id_, resp.path_);
}

} // namespace fbx::xserver
