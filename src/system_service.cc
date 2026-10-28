#include <sys/param.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/vmmeter.h>
#include <sys/resource.h>
#include <vm/vm_param.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <ctime>
#include <cstdio>
#include "xserver/system_service.h"
#include "xserver/ipc_socket.h"

namespace fbx::xserver {

uint32_t SystemService::frame_count_ = 0;

shared::SystemInfo SystemService::getSystemStats() noexcept {
    shared::SystemInfo info{};

    // 1. Get CPU Usage (kern.cp_time)
    long cp_time[CPUSTATES];
    size_t cp_time_len = sizeof(cp_time);
    if (sysctlbyname("kern.cp_time", &cp_time, &cp_time_len, NULL, 0) == 0) {
        long total = 0;
        for (int i = 0; i < CPUSTATES; i++) total += cp_time[i];
        if (total > 0) {
            info.cpu_usage_ = static_cast<uint32_t>(100 * (total - cp_time[CP_IDLE]) / total);
        }
    }

    // 2. Get Memory Info
    u_int page_count;
    size_t len = sizeof(page_count);
    if (sysctlbyname("vm.stats.vm.v_page_count", &page_count, &len, NULL, 0) == 0) {
        info.mem_total_mb_ = (static_cast<uint64_t>(page_count) * getpagesize()) / (1024 * 1024);
    }
    
    u_int free_count;
    len = sizeof(free_count);
    if (sysctlbyname("vm.stats.vm.v_free_count", &free_count, &len, NULL, 0) == 0) {
        uint32_t free_mb = (static_cast<uint64_t>(free_count) * getpagesize()) / (1024 * 1024);
        info.mem_used_mb_ = info.mem_total_mb_ - free_mb;
    }

    // 3. Get Battery Info
    int life = 0;
    size_t battery_len = sizeof(life);
    if (sysctlbyname("hw.acpi.battery.life", &life, &battery_len, NULL, 0) == 0) {
        info.battery_percentage_ = (uint32_t)life;
    } else {
        info.battery_percentage_ = 100;
    }

    int state = 0;
    battery_len = sizeof(state);
    if (sysctlbyname("hw.acpi.battery.state", &state, &battery_len, NULL, 0) == 0) {
        info.is_charging_ = (state == 2) ? 1 : 0; // State 2 = Charging
    }

    // 4. Get Uptime
    struct timespec ts;
    if (clock_gettime(CLOCK_UPTIME, &ts) == 0) {
        int hours = ts.tv_sec / 3600;
        int minutes = (ts.tv_sec % 3600) / 60;
        int seconds = ts.tv_sec % 60;
        std::snprintf(info.uptime_, sizeof(info.uptime_), "%02d:%02d:%02d", hours, minutes, seconds);
    }

    info.frame_count_ = frame_count_;
    info.gpu_temp_ = 0;  // no GPU temperature sensor is exposed (0 = unknown)

    return info;
}

bool SystemService::ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept {
    if (msg.opcode_ == 0x01) { // GetSystemInfo
        shared::XBusMessage response = msg;
        shared::SystemInfo stats = getSystemStats();
        std::memcpy(response.payload_, &stats, sizeof(stats));
        
        std::span<const uint8_t> resp_span(reinterpret_cast<const uint8_t*>(&response), sizeof(response));
        (void)client->sendMessage(resp_span);
        return true;
    }
    return false;
}

} // namespace fbx::xserver
