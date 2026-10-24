#pragma once

#include "shared/protocol.h"
#include <cstring>
#include <string>
#include <vector>

// Pure page-builder for com.freebsdx.DisplayArrange. The live output list comes
// from the compositor (window_stack) via a provider; this just slices it into
// the wire page. Dependency-free / unit-testable.
namespace fbx::xserver::disparrange {

struct ArrangeOutput {
    uint32_t    width = 0, height = 0, refresh_mhz = 0;
    bool        primary = false;
    bool        mirroring = false;
    std::string name;
    std::string make;
    std::string model;
};

// One output per page (matches DisplayOutputPage's single-output layout).
inline shared::DisplayOutputPage buildPage(const std::vector<ArrangeOutput>& outs, uint32_t page) {
    shared::DisplayOutputPage pg{};
    pg.page_  = page;
    pg.total_ = static_cast<uint32_t>(outs.size());
    if (page < outs.size()) {
        const ArrangeOutput& o = outs[page];
        pg.valid_       = 1;
        pg.output_id_   = page;
        pg.is_primary_  = o.primary ? 1u : 0u;
        pg.mirroring_   = o.mirroring ? 1u : 0u;
        pg.width_       = o.width;
        pg.height_      = o.height;
        pg.refresh_mhz_ = o.refresh_mhz;
        std::strncpy(pg.name_,  o.name.c_str(),  sizeof(pg.name_) - 1);
        std::strncpy(pg.make_,  o.make.c_str(),  sizeof(pg.make_) - 1);
        std::strncpy(pg.model_, o.model.c_str(), sizeof(pg.model_) - 1);
    } else {
        pg.valid_ = 0;
    }
    pg.has_more_ = (page + 1 < outs.size()) ? 1u : 0u;
    return pg;
}

}  // namespace fbx::xserver::disparrange
