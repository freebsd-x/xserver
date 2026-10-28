#include "xserver/gpu_monitor_service.h"
#include "xserver/gpu_info.h"
#include "xserver/async_shell.h"

#include <climits>
#include <cstring>
#include <string>

namespace fbx::xserver {

namespace {

// The GPU doesn't change while we run; GpuStats is polled every 2 s, so read
// pciconf once.
const std::string& gpuName() {
    // posix_spawn, not popen (popen deadlocks the threaded compositor).
    static const std::string name = [] {
        std::string n = gpu::parseGpuName(spawnCapture("pciconf -lv 2>/dev/null"));
        return n.empty() ? std::string("Unknown GPU") : n;
    }();
    return name;
}

}  // namespace

void GpuMonitorService::handleMessage(const shared::XBusMessage& msg,
                                      std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::GpuMonitorOpcode;
    if (static_cast<O>(msg.opcode_) != O::GetStats) return;

    shared::GpuStatsDetail st{};
    std::strncpy(st.name_, gpuName().c_str(), sizeof(st.name_) - 1);
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

void GpuStatsService::handleMessage(const shared::XBusMessage& msg,
                                    std::shared_ptr<IpcSocket> client) noexcept {
    if (static_cast<shared::GpuOpcode>(msg.opcode_) != shared::GpuOpcode::GetStats) return;
    shared::GpuStats st{};
    std::strncpy(st.name_, gpuName().c_str(), sizeof(st.name_) - 1);
    st.temp_c_ = INT32_MIN;   // no per-GPU thermal sensor on an Intel iGPU

    shared::XBusMessage reply{};
    reply.transaction_id_ = msg.transaction_id_;
    std::strncpy(reply.service_name_, "com.freebsdx.GpuStats", sizeof(reply.service_name_) - 1);
    reply.opcode_ = msg.opcode_;
    std::memcpy(reply.payload_, &st, sizeof(st));
    if (client) client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

}  // namespace fbx::xserver
