#include "xserver/frame_perf_service.h"
#include "xserver/frame_perf.h"

#include <array>
#include <cstring>
#include <mutex>
#include <vector>

namespace fbx::xserver {

namespace {
constexpr int kWindow = 64;

std::mutex              g_mu;
std::array<float, kWindow> g_ring{};
int                     g_head = 0;    // next write slot
int                     g_count = 0;   // valid samples (≤ kWindow)
uint64_t                g_total_frames = 0;
float                   g_last_ms = 0.0f;

// Snapshot the ring in oldest→newest order (caller holds g_mu).
std::vector<float> snapshotLocked() {
    std::vector<float> v;
    v.reserve(static_cast<size_t>(g_count));
    int start = (g_count < kWindow) ? 0 : g_head;   // oldest slot
    for (int i = 0; i < g_count; ++i)
        v.push_back(g_ring[(start + i) % kWindow]);
    return v;
}
}  // namespace

void FramePerfService::publish(float last_frame_ms, uint64_t total_frames) noexcept {
    std::lock_guard<std::mutex> lk(g_mu);
    g_ring[g_head] = last_frame_ms;
    g_head = (g_head + 1) % kWindow;
    if (g_count < kWindow) ++g_count;
    g_total_frames = total_frames;
    g_last_ms = last_frame_ms;
}

void FramePerfService::reset() noexcept {
    std::lock_guard<std::mutex> lk(g_mu);
    g_head = 0; g_count = 0; g_total_frames = 0; g_last_ms = 0.0f;
    g_ring.fill(0.0f);
}

void FramePerfService::handleMessage(const shared::XBusMessage& msg,
                                     std::shared_ptr<IpcSocket> client) noexcept {
    if (static_cast<shared::FramePerfOpcode>(msg.opcode_) != shared::FramePerfOpcode::GetStats)
        return;

    shared::FramePerfState st;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        st = frameperf::computeState(snapshotLocked(), g_total_frames, g_last_ms);
    }

    shared::XBusMessage reply{};
    reply.transaction_id_ = msg.transaction_id_;
    std::strncpy(reply.service_name_, "com.freebsdx.FramePerf", sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(shared::FramePerfOpcode::GetStats);
    std::memcpy(reply.payload_, &st, sizeof(st));
    if (client) client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

}  // namespace fbx::xserver
