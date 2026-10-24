#pragma once

#include "shared/protocol.h"

#include <cstring>
#include <vector>

#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>

// Shared KERN_PROC process enumeration for the services that rank processes
// (Process, BatteryDrain, Energy). Inline (does sysctl — include only from .cc)
// and kept separate from the pure proc_list.h so unit tests of that header pull
// no syscalls. Returns ProcessEntry{name,pid,cpu_pct10,rss_kb}; consumers use
// whatever fields they need.
namespace fbx::xserver::proc {

inline std::vector<shared::ProcessEntry> enumerateProcesses() {
    std::vector<shared::ProcessEntry> out;
    // KERN_PROC_ALL takes no argument, so the mib is length 3 (a 4th element
    // yields ENOTDIR — a bug that silently emptied earlier callers).
    int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
    size_t len = 0;
    if (sysctl(mib, 3, nullptr, &len, nullptr, 0) != 0) return out;
    std::vector<struct kinfo_proc> kp(len / sizeof(struct kinfo_proc) + 16);
    len = kp.size() * sizeof(struct kinfo_proc);
    if (sysctl(mib, 3, kp.data(), &len, nullptr, 0) != 0) return out;

    size_t count = len / sizeof(struct kinfo_proc);
    long pagesize = ::getpagesize();
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const struct kinfo_proc& k = kp[i];
        shared::ProcessEntry e{};
        std::strncpy(e.name_, k.ki_comm, sizeof(e.name_) - 1);
        e.pid_       = static_cast<uint32_t>(k.ki_pid);
        e.cpu_pct10_ = static_cast<uint32_t>(
            static_cast<uint64_t>(k.ki_pctcpu) * 1000 / FSCALE);   // CPU% ×10
        e.rss_kb_    = static_cast<uint32_t>(
            static_cast<uint64_t>(k.ki_rssize) * pagesize / 1024);
        out.push_back(e);
    }
    return out;
}

}  // namespace fbx::xserver::proc
