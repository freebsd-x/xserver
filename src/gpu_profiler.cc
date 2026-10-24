#include "xserver/gpu_profiler.h"
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace fbx::xserver {

bool GpuProfiler::init(VkDevice device, VkPhysicalDevice physical_device) noexcept {
    device_ = device;

    // Nanoseconds per timestamp tick — device-specific constant.
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_device, &props);
    ns_per_tick_ = static_cast<double>(props.limits.timestampPeriod);

    // Two queries per frame: slot 0 = begin, slot 1 = end.
    VkQueryPoolCreateInfo qpci{
        VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0,
        VK_QUERY_TYPE_TIMESTAMP, 2, 0
    };
    if (vkCreateQueryPool(device_, &qpci, nullptr, &query_pool_) != VK_SUCCESS) {
        std::cerr << "[GpuProfiler] Failed to create timestamp query pool\n";
        return false;
    }

    // Open append-mode log.
    const char* home = std::getenv("HOME");
    std::string path = home ? std::string(home) + "/.freebsdx-perf.log"
                            : "/tmp/freebsdx-perf.log";
    log_.open(path, std::ios::app);
    if (log_.is_open())
        log_ << "# FreeBSD-X GPU profiler started — ns/tick=" << ns_per_tick_ << "\n";

    std::cout << "[GpuProfiler] Timestamp pool ready; logging to " << path << "\n";
    return true;
}

void GpuProfiler::destroy() noexcept {
    if (query_pool_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, query_pool_, nullptr);
        query_pool_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
}

void GpuProfiler::beginFrame(VkCommandBuffer cmd) noexcept {
    if (!isValid()) return;
    // Inline reset (Vulkan 1.2+). This is safe because we called vkQueueWaitIdle
    // before freeing the previous command buffer, so queries are no longer in flight.
    vkCmdResetQueryPool(cmd, query_pool_, 0, 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool_, 0);
}

void GpuProfiler::endFrame(VkCommandBuffer cmd) noexcept {
    if (!isValid()) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool_, 1);
}

void GpuProfiler::resolveFrame() noexcept {
    if (!isValid()) return;

    uint64_t timestamps[2] = {0, 0};
    VkResult res = vkGetQueryPoolResults(
        device_, query_pool_, 0, 2,
        sizeof(timestamps), timestamps, sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    if (res != VK_SUCCESS) return;

    double elapsed_ns = static_cast<double>(timestamps[1] - timestamps[0]) * ns_per_tick_;
    float ms = static_cast<float>(elapsed_ns * 1.0e-6);
    last_ms_ = ms;

    // Update rolling history.
    history_[head_] = ms;
    head_ = (head_ + 1) % kHistorySize;
    if (count_ < kHistorySize) ++count_;

    ++frame_count_;

    // Periodic log write.
    if (log_.is_open() && (frame_count_ % kLogInterval) == 0) {
        log_ << "frame=" << frame_count_
             << " gpu=" << ms
             << "ms avg=" << averageMs() << "ms\n";
        log_.flush();
    }
}

float GpuProfiler::averageMs() const noexcept {
    if (count_ == 0) return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < count_; ++i) sum += history_[i];
    return sum / static_cast<float>(count_);
}

} // namespace fbx::xserver
