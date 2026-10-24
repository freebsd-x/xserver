#pragma once

#include "shared/protocol.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// Pure energy-usage attribution for com.freebsdx.Energy. Like BatteryDrain, but
// the wire shape is per-app CPU% + a 4-level impact + wakeups/sec. FreeBSD has
// no per-process wakeup counter, so wakeups_sec_ is 0; impact/CPU come from the
// KERN_PROC CPU% (the enumeration lives in the service). Dependency-free /
// unit-testable.
namespace fbx::xserver::energy {

// 0=low, 1=medium, 2=high, 3=very high — from a process's CPU%.
inline uint32_t impactFor(uint32_t cpu_pct) noexcept {
    if (cpu_pct >= 50) return 3;
    if (cpu_pct >= 20) return 2;
    if (cpu_pct >= 5)  return 1;
    return 0;
}

// Rank processes by CPU% descending into EnergyEntry rows; zero-CPU processes
// are dropped (no measurable energy use). cpu_avg_pct_ = cpu_pct10/10 (clamped
// 100); wakeups_sec_ = 0 (unavailable on FreeBSD).
inline std::vector<shared::EnergyEntry>
computeUsage(std::vector<shared::ProcessEntry> procs) {
    std::stable_sort(procs.begin(), procs.end(),
        [](const shared::ProcessEntry& a, const shared::ProcessEntry& b) {
            return a.cpu_pct10_ > b.cpu_pct10_;
        });

    std::vector<shared::EnergyEntry> out;
    for (const auto& p : procs) {
        if (p.cpu_pct10_ == 0) continue;
        shared::EnergyEntry e{};
        std::strncpy(e.app_name_, p.name_, sizeof(e.app_name_) - 1);
        uint32_t cpu = std::min(p.cpu_pct10_ / 10u, 100u);
        e.cpu_avg_pct_ = cpu;
        e.impact_      = impactFor(cpu);
        e.wakeups_sec_ = 0;   // no per-process wakeup counter on FreeBSD
        out.push_back(e);
    }
    return out;
}

inline shared::EnergyPage buildPage(const std::vector<shared::EnergyEntry>& all,
                                    uint32_t page) noexcept {
    shared::EnergyPage pg{};
    pg.page_  = page;
    pg.total_ = static_cast<uint32_t>(all.size());
    uint32_t start = page * 3, n = 0;
    for (; n < 3 && start + n < all.size(); ++n) pg.entries_[n] = all[start + n];
    pg.count_    = n;
    pg.has_more_ = (start + n < all.size()) ? 1u : 0u;
    return pg;
}

}  // namespace fbx::xserver::energy
