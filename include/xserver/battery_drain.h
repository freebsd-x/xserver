#pragma once

#include "shared/protocol.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// Pure energy-impact attribution for com.freebsdx.BatteryDrain. FreeBSD has no
// per-process power accounting (powerd(8) only scales CPU frequency), so we
// approximate each app's battery drain by its share of total CPU time — the
// dominant energy proxy on a laptop, and what "energy impact" means on other
// OSes. Dependency-free / unit-testable; the KERN_PROC enumeration lives in the
// service.
namespace fbx::xserver::batdrain {

// energy_pct thresholds → impact level.
inline uint32_t impactFor(uint32_t energy_pct) noexcept {
    if (energy_pct >= 20) return static_cast<uint32_t>(shared::BatteryDrainImpact::High);
    if (energy_pct >= 5)  return static_cast<uint32_t>(shared::BatteryDrainImpact::Medium);
    return static_cast<uint32_t>(shared::BatteryDrainImpact::Low);
}

// Rank processes by CPU share into drain entries. Zero-CPU processes are dropped
// (no measurable drain). energy_pct is the process's CPU% as a fraction of the
// summed CPU% across all processes, so the visible entries roughly sum to 100.
inline std::vector<shared::BatteryDrainEntry>
computeDrain(std::vector<shared::ProcessEntry> procs) {
    uint64_t total = 0;
    for (const auto& p : procs) total += p.cpu_pct10_;

    std::stable_sort(procs.begin(), procs.end(),
        [](const shared::ProcessEntry& a, const shared::ProcessEntry& b) {
            return a.cpu_pct10_ > b.cpu_pct10_;
        });

    std::vector<shared::BatteryDrainEntry> out;
    for (const auto& p : procs) {
        if (p.cpu_pct10_ == 0) continue;                 // no measurable drain
        shared::BatteryDrainEntry e{};
        std::strncpy(e.app_name_, p.name_, sizeof(e.app_name_) - 1);
        e.energy_pct_ = total ? static_cast<uint32_t>(
                            (static_cast<uint64_t>(p.cpu_pct10_) * 100 + total / 2) / total) : 0;
        e.impact_ = impactFor(e.energy_pct_);
        e.pid_    = p.pid_;
        out.push_back(e);
    }
    return out;
}

// Slice a drain list into a wire page (3 entries/page).
inline shared::BatteryDrainPage buildPage(const std::vector<shared::BatteryDrainEntry>& all,
                                          uint32_t page) noexcept {
    shared::BatteryDrainPage pg{};
    pg.page_  = page;
    pg.total_ = static_cast<uint32_t>(all.size());
    uint32_t start = page * 3, n = 0;
    for (; n < 3 && start + n < all.size(); ++n) pg.entries_[n] = all[start + n];
    pg.count_    = n;
    pg.has_more_ = (start + n < all.size()) ? 1u : 0u;
    return pg;
}

}  // namespace fbx::xserver::batdrain
