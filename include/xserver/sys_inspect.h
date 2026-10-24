#pragma once

#include "shared/protocol.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Pure helpers for the read-only system-inspection services (com.freebsdx.SysInfo
// and com.freebsdx.MountedVolumes). Dependency-free so they can be unit-tested
// without sysctl / getmntinfo.
namespace fbx::xserver::sysinspect {

// "FreeBSD" + " 14.2-RELEASE-p3" → "FreeBSD 14.2-RELEASE-p3"; tolerant of an
// empty part.
inline std::string osVersion(std::string_view ostype, std::string_view osrelease) noexcept {
    std::string t(ostype), r(osrelease);
    if (t.empty()) return r;
    if (r.empty()) return t;
    return t + " " + r;
}

// Slice a volume list into one wire page (3 entries/page), mirroring the other
// paged services.
inline shared::VolumePage buildVolumePage(const std::vector<shared::VolumeEntry>& all,
                                          uint32_t page) noexcept {
    shared::VolumePage pg{};
    pg.page_  = page;
    pg.total_ = static_cast<uint32_t>(all.size());
    uint32_t start = page * 3;
    uint32_t n = 0;
    for (; n < 3 && start + n < all.size(); ++n) pg.entries_[n] = all[start + n];
    pg.count_    = n;
    pg.has_more_ = (start + n < all.size()) ? 1u : 0u;
    return pg;
}

}  // namespace fbx::xserver::sysinspect
