#pragma once
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <memory>

namespace fbx::xserver {

class PowerProfileService {
public:
    static void init() noexcept;   // read current CPU freq range at startup
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;
    // Called by PowerService when AC ↔ battery transition detected
    static void onAcChanged(bool on_ac) noexcept;

private:
    static shared::PowerProfileLevel applyProfile(shared::PowerProfileLevel level) noexcept;
    static shared::PowerProfileState getState() noexcept;
    static void readCpuFreqRange() noexcept;   // populate min_/max_ from sysctl

    static shared::PowerProfileLevel current_;
    static uint32_t cpu_min_mhz_;
    static uint32_t cpu_max_mhz_;
    static uint32_t cpu_cur_mhz_;
};

} // namespace fbx::xserver
