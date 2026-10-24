#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <cstdint>

namespace fbx::xserver {

// com.freebsdx.FramePerf — compositor frame-timing stats (GetStats →
// FramePerfState). The compositor calls publish() once per frame (after
// GpuProfiler::resolveFrame) with the last GPU frame time + cumulative count;
// the service keeps a rolling window and derives avg/p99/jank/recent on demand.
// Headless / no compositor → all zeros.
class FramePerfService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    // Per-frame feed from the render loop (thread-safe).
    static void publish(float last_frame_ms, uint64_t total_frames) noexcept;

    // Clear the rolling window (tests).
    static void reset() noexcept;
};

}  // namespace fbx::xserver
