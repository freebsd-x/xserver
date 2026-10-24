#include "xserver/gpu_monitor_service.h"
#include "xserver/gpu_info.h"
#include "xserver/async_shell.h"

#include <cstring>
#include <string>

namespace fbx::xserver {

void GpuMonitorService::handleMessage(const shared::XBusMessage& msg,
                                      std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::GpuMonitorOpcode;
    if (static_cast<O>(msg.opcode_) != O::GetStats) return;

    // posix_spawn, not popen (popen deadlocks the threaded compositor).
    std::string name = gpu::parseGpuName(spawnCapture("pciconf -lv 2>/dev/null"));

    shared::GpuStatsDetail st{};
    std::strncpy(st.name_, name.empty() ? "Unknown GPU" : name.c_str(), sizeof(st.name_) - 1);
    st.util_pct_      = 0;   // not exposed for Intel iGPU on FreeBSD
    st.vram_used_mb_  = 0;   // iGPU shares system RAM — no dedicated VRAM counter
    st.vram_total_mb_ = 0;
    st.temp_c_        = 0;   // no per-GPU thermal sensor
    st.fan_rpm_       = 0;

    shared::XBusMessage reply{};
    reply.transaction_id_ = msg.transaction_id_;
    std::strncpy(reply.service_name_, "com.freebsdx.GpuMonitor", sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(O::GetStats);
    std::memcpy(reply.payload_, &st, sizeof(st));
    if (client) client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

}  // namespace fbx::xserver
