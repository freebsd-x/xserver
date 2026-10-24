#pragma once

#include "shared/protocol.h"
#include <memory>
#include <cstddef>

namespace fbx::xserver {

class IpcSocket;

class VpnService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    static shared::VpnStatus getStatus() noexcept;

private:
    static void connectBackground(const char* iface) noexcept;
    static void disconnectBackground(const char* iface) noexcept;
    static bool importConfig(const char* src_path,
                             char* out_iface, size_t iface_sz) noexcept;
};

} // namespace fbx::xserver
