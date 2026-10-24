#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <mutex>
#include <vector>

namespace fbx::xserver {

class TrayService {
public:
    static void init() noexcept;

    // Called by dispatcher — returns entry with assigned id_
    static shared::TrayEntry     registerIcon(const shared::TrayRegisterRequest& req,
                                              std::shared_ptr<IpcSocket> owner) noexcept;
    static void                  unregister(uint32_t id) noexcept;
    static void                  setIcon(const shared::TrayUpdateRequest& req) noexcept;
    static void                  setTooltip(const shared::TrayUpdateRequest& req) noexcept;
    static void                  setStatus(const shared::TrayStatusRequest& req) noexcept;
    static shared::TrayEntryPage listEntries(uint32_t page) noexcept;

    // Called when xshell user clicks a tray icon — routes Click back to owner
    static void dispatchClick(uint32_t id) noexcept;

private:
    struct Entry {
        shared::TrayEntry         data;
        std::weak_ptr<IpcSocket>  owner;
    };

    static std::mutex        mutex_;
    static std::vector<Entry> entries_;
    static uint32_t           next_id_;

    static void broadcastChanged() noexcept;
};

} // namespace fbx::xserver
