#pragma once

#include "shared/protocol.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Pure frame-timing statistics for com.freebsdx.FramePerf. Given a window of
// recent per-frame GPU times (ms, oldest→newest), compute the FramePerfState the
// shell renders. Dependency-free / unit-testable; the live samples come from the
// compositor's GpuProfiler (published per frame into FramePerfService).
namespace fbx::xserver::frameperf {

constexpr float kJankMs = 16.6f;   // >1 frame at 60 Hz (float: avoids the
                                   // 16.6f-promoted-to-double boundary surprise)

// Build a FramePerfState from a recent-frame window (ms, oldest→newest),
// cumulative frame count, and the last GPU-submit time (ms).
inline shared::FramePerfState computeState(const std::vector<float>& ms,
                                           uint64_t total_frames,
                                           float last_submit_ms) {
    shared::FramePerfState st{};
    st.total_frames_ = static_cast<uint32_t>(total_frames);
    st.gpu_submit_us_ = static_cast<uint32_t>(last_submit_ms * 1000.0f + 0.5f);
    st.vsync_misses_ = 0;   // not tracked by the timestamp profiler

    const int n = static_cast<int>(ms.size());
    if (n > 0) {
        double sum = 0.0;
        uint32_t jank = 0;
        for (float v : ms) { sum += v; if (v > kJankMs) ++jank; }
        st.avg_frame_us_ = static_cast<uint64_t>((sum / n) * 1000.0 + 0.5);
        st.jank_count_   = jank;

        std::vector<float> sorted(ms);
        std::sort(sorted.begin(), sorted.end());
        int idx = static_cast<int>(std::ceil(0.99 * n)) - 1;
        if (idx < 0) idx = 0;
        if (idx >= n) idx = n - 1;
        st.p99_frame_us_ = static_cast<uint64_t>(sorted[static_cast<size_t>(idx)] * 1000.0 + 0.5);
    }

    // recent_[16]: the last 16 frames as whole ms (0–255), newest at index 15.
    for (int i = 0; i < 16; ++i) {
        int src = n - 16 + i;
        if (src >= 0) {
            long v = std::lround(ms[static_cast<size_t>(src)]);
            st.recent_[i] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    return st;
}

}  // namespace fbx::xserver::frameperf
