#pragma once

#include "shared/protocol.h"
#include <memory>
#include <string>

namespace fbx::xserver {

class IpcSocket;

// PkgSigningService — trust store management and .xpkg signature verification.
//
// Trust store  : ~/.freebsdx-pkg-trust/keys/  (one .pub/.asc per key)
// Policy file  : ~/.freebsdx-pkg-trust/policy
// Sidecar files: <pkg>.xpkg.minisig  (minisign) or  <pkg>.xpkg.sig  (GPG detached)
//
// AppManagerService calls VerifyPackage() before installing any .xpkg archive.
// When policy.require_signed_ == 1 the install is aborted on verification failure.
class PkgSigningService {
public:
    struct VerifyResult {
        bool        ok;
        std::string message;
    };

    // Verify a .xpkg file against the trust store.
    static VerifyResult VerifyPackage(const std::string& xpkg_path) noexcept;

    // Read current signing policy from disk.
    static shared::PkgSigningPolicy GetPolicy() noexcept;

    // Handle com.freebsdx.PackageSigning XBus messages.
    static void ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

private:
    static std::string trustDir() noexcept;   // ~/.freebsdx-pkg-trust
    static std::string keysDir()  noexcept;   // ~/.freebsdx-pkg-trust/keys
    static void broadcastChanged() noexcept;
};

} // namespace fbx::xserver
