#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.GpuMonitor — GPU stats (GpuMonitorOpcode::GetStats → GpuStatsDetail).
// The GPU name is real (pciconf -lv, display-class device); utilisation, VRAM,
// temperature and fan are not exposed for an Intel iGPU on FreeBSD → reported 0.
class GpuMonitorService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.GpuStats — the AC card's 2 s poll (GpuOpcode::GetStats →
// GpuStats). Same source as GpuMonitor: the name is real, temp_c_ is
// INT32_MIN (no sensor) and the other counters are 0.
class GpuStatsService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
