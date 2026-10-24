#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <mutex>
#include <vector>

namespace fbx::xserver {

// com.freebsdx.FileChooser
// Queues open/save requests from sandboxed apps, broadcasts Show to xshell,
// and routes the xshell Respond back to the original requester as a Response
// broadcast so all listeners (including the requesting app) can match by
// request_id_.
class FileChooserService {
public:
    static void init() noexcept;

    // Called by dispatcher for OpenFile/OpenFiles/SaveFile — assigns request_id,
    // broadcasts Show to xshell, and sends an immediate ack (with request_id)
    // back to the requesting app.
    static void requestOpen(uint32_t opcode, uint32_t tid,
                            const shared::FileChooserRequest& req,
                            std::shared_ptr<IpcSocket> client) noexcept;

    // Called by dispatcher for Respond (xshell sends the user's selection) —
    // broadcasts FileChooserOpcode::Response to all clients.
    static void handleRespond(const shared::FileChooserResponse& resp) noexcept;

private:
    struct PendingRequest {
        uint32_t                 id;
        std::weak_ptr<IpcSocket> requester;
        bool                     multi_select;
        bool                     save_mode;
    };

    static std::mutex                    mutex_;
    static std::vector<PendingRequest>   pending_;
    static uint32_t                      next_id_;
};

} // namespace fbx::xserver
