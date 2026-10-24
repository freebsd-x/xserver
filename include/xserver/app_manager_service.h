#pragma once

#include "shared/protocol.h"
#include <memory>
#include <string>

namespace fbx::xserver {

class IpcSocket;

// AppManagerService — install, uninstall, and enumerate .xapp bundles.
//
// Package format (.xpkg):
//   A gzip-compressed tar archive whose root contains exactly one .xapp
//   directory.  Created with:
//     tar -czf AppName-1.0.0.xpkg AppName.xapp/
//   Extracted with:
//     tar -xzf foo.xpkg -C <apps_dir>
//
// The service also accepts a bare .xapp directory path for local development
// installs (same behaviour as `xpkg install <dir>`).
//
// Install progress is broadcast to all connected clients as a series of
// AppInstallProgress messages (AppManagerOpcode::InstallProgress).
class AppManagerService {
public:
    // Directory that contains installed .xapp bundles.
    // Defaults to "./apps"; overridden at startup from main.cc.
    static void setAppsDir(const std::string& dir) noexcept;
    [[nodiscard]] static const std::string& appsDir() noexcept;

    // Handle a com.freebsdx.AppManager XBus message.
    static void ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    // Remote catalog operations.
    static shared::AppManagerCatalogState updateCatalog() noexcept;
    static shared::AppManagerRemotePage   listRemote(uint32_t page) noexcept;
    static shared::AppManagerRemotePage   searchRemote(const shared::AppManagerSearchRequest& req) noexcept;
    static void                           installRemote(const shared::AppManagerRemoteInstallRequest& req,
                                                        std::shared_ptr<IpcSocket> requester) noexcept;

private:
    // Broadcast a progress update to every connected client.
    static void broadcastProgress(const std::string& identifier,
                                  uint32_t percent,
                                  uint32_t status,        // 0=running,1=ok,2=err
                                  const std::string& message) noexcept;

    static inline std::string apps_dir_{"./apps"};
};

} // namespace fbx::xserver
