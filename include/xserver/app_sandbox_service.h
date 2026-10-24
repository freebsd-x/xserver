#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include "xserver/permission_store.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.AppSandbox — backend for the per-app permission panel and the
// runtime permission enforcement in the dispatcher. Wraps a PermissionStore.
//   ListApps(page)     → AppSandboxPage (one app per page entry)
//   SetPermission(req) → grant/revoke a capability, persist, broadcast
//   PermsChanged       → broadcast so the shell re-fetches
class AppSandboxService {
public:
    static void init() noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    // Enforcement hook for the dispatcher: may `app` use `cap`? Consulted for
    // sandboxed clients before a sensitive operation.
    static bool isAllowed(const std::string& app,
                          PermissionStore::Capability cap) noexcept;

private:
    static void sendPage(IpcSocket* client, uint32_t tid, uint32_t page) noexcept;
    static PermissionStore& store() noexcept;
};

}  // namespace fbx::xserver
