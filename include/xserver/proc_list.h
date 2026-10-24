#pragma once

#include "shared/protocol.h"
#include <algorithm>
#include <cstdint>
#include <vector>

// Pure helpers for com.freebsdx.Process: sort processes by CPU descending and
// slice into wire pages. Dependency-free / unit-testable (the sysctl enumeration
// lives in the service).
namespace fbx::xserver::proc {

// Sort by CPU% descending (stable, so equal-CPU processes keep enumeration order).
inline void sortByCpuDesc(std::vector<shared::ProcessEntry>& v) noexcept {
    std::stable_sort(v.begin(), v.end(),
        [](const shared::ProcessEntry& a, const shared::ProcessEntry& b) {
            return a.cpu_pct10_ > b.cpu_pct10_;
        });
}

inline shared::ProcessPage buildPage(const std::vector<shared::ProcessEntry>& all,
                                     uint32_t page) noexcept {
    shared::ProcessPage pg{};
    pg.page_  = page;
    pg.total_ = static_cast<uint32_t>(all.size());
    uint32_t start = page * 3;
    uint32_t n = 0;
    for (; n < 3 && start + n < all.size(); ++n) pg.entries_[n] = all[start + n];
    pg.count_    = n;
    pg.has_more_ = (start + n < all.size()) ? 1u : 0u;
    return pg;
}

}  // namespace fbx::xserver::proc
