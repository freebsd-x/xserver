#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <vector>

namespace fbx::xserver {

// com.freebsdx.Thermal — temperature sensors (and best-effort fan control).
//
// GetZones(page) reports one thermal zone per page (CPU cores via
// dev.cpu.N.temperature, ACPI zones via hw.acpi.thermal.tzN.temperature).
// SetFanMode is accepted but a real change needs hardware/root the compositor
// lacks, so it degrades to an ack.
class ThermalService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    struct Zone { uint32_t temp_c; char label[48]; };
    // Read the current sensors (test/inspection seam).
    [[nodiscard]] static std::vector<Zone> readZones() noexcept;
};

}  // namespace fbx::xserver
