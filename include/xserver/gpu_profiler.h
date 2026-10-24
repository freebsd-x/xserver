#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <fstream>

namespace fbx::xserver {

// GPU frame-time profiler using a VkQueryPool (TIMESTAMP type).
//
// Usage per frame:
//   profiler.beginFrame(cmd);    // immediately after vkBeginCommandBuffer
//   ... render commands ...
//   profiler.endFrame(cmd);      // before vkEndCommandBuffer
//   vkQueueSubmit(...);
//   vkQueueWaitIdle(...);
//   profiler.resolveFrame();     // read back timestamps, update stats, log
//
// The VK_EXT_debug_utils render-pass labels inserted by the compositor let
// GPU profilers (RenderDoc, NSight) see named regions alongside these timings.
class GpuProfiler {
public:
    static constexpr int kHistorySize = 64;   // rolling-average window
    static constexpr int kLogInterval = 60;   // write to log every N frames

    GpuProfiler() = default;
    ~GpuProfiler() noexcept { destroy(); }
    GpuProfiler(const GpuProfiler&) = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;

    // Create the VkQueryPool and open ~/.freebsdx-perf.log.
    bool init(VkDevice device, VkPhysicalDevice physical_device) noexcept;
    void destroy() noexcept;

    // Record a TIMESTAMP at the top of the command buffer.
    void beginFrame(VkCommandBuffer cmd) noexcept;

    // Record a TIMESTAMP at the bottom of the last command in the buffer.
    void endFrame(VkCommandBuffer cmd) noexcept;

    // Must be called after vkQueueWaitIdle to read back results.
    void resolveFrame() noexcept;

    // Rolling average of the last kHistorySize frames (milliseconds).
    float averageMs() const noexcept;

    // Raw last-frame GPU time (milliseconds).
    float lastMs() const noexcept { return last_ms_; }

    uint64_t frameCount() const noexcept { return frame_count_; }
    bool     isValid()    const noexcept { return query_pool_ != VK_NULL_HANDLE; }

private:
    VkDevice    device_{VK_NULL_HANDLE};
    VkQueryPool query_pool_{VK_NULL_HANDLE};
    double      ns_per_tick_{1.0}; // nanoseconds per timestamp tick (from device limits)

    float    history_[kHistorySize]{};
    int      head_{0};
    int      count_{0};
    float    last_ms_{0.0f};
    uint64_t frame_count_{0};

    std::ofstream log_;
};

} // namespace fbx::xserver
