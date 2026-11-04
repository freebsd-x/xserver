#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <mutex>
#include <vector>

namespace fbx::xserver {

// com.freebsdx.FileChooser
// Queues open/save requests from apps, asks the shell (and only the shell) to
// present the picker, and routes its Respond to the requester alone: which file
// someone chose is nobody else's business. OpenFd/SaveFd are the powerbox — the
// compositor opens the file and the reply carries the descriptor.
class FileChooserService {
public:
    static void init() noexcept;

    // Called by dispatcher for OpenFile/OpenFiles/SaveFile/OpenFd/SaveFd — assigns
    // request_id and asks the shell to show the picker. The path requests get an
    // immediate ack (with request_id); the fd requests get their one reply when
    // the user has answered.
    static void requestOpen(uint32_t opcode, uint32_t tid,
                            const shared::FileChooserRequest& req,
                            std::shared_ptr<IpcSocket> client) noexcept;

    // Called by dispatcher for Respond (the shell sends the user's selection;
    // apps are refused before this) — answers the requester.
    static void handleRespond(const shared::FileChooserResponse& resp) noexcept;

private:
    struct PendingRequest {
        uint32_t                 id;
        std::weak_ptr<IpcSocket> requester;
        bool                     multi_select;
        bool                     save_mode;
        bool                     wants_fd;   // OpenFd / SaveFd: reply with a descriptor
        uint32_t                 tid;        // ...to this Call
    };

    static std::mutex                    mutex_;
    static std::vector<PendingRequest>   pending_;
    static uint32_t                      next_id_;
};

} // namespace fbx::xserver
