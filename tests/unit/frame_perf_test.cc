// Pure frame-timing statistics (avg/p99/jank/recent window).

#include "xserver/frame_perf.h"
#include <gtest/gtest.h>
#include <vector>

using namespace fbx::xserver::frameperf;

TEST(FramePerf, EmptyWindowIsAllZero) {
    auto st = computeState({}, 0, 0.0f);
    EXPECT_EQ(st.avg_frame_us_, 0u);
    EXPECT_EQ(st.p99_frame_us_, 0u);
    EXPECT_EQ(st.jank_count_, 0u);
    EXPECT_EQ(st.total_frames_, 0u);
    EXPECT_EQ(st.recent_[15], 0);
}

TEST(FramePerf, AverageAndTotalsAndSubmit) {
    std::vector<float> ms{10.0f, 20.0f, 30.0f};   // mean 20 ms
    auto st = computeState(ms, 1234, 8.5f);
    EXPECT_EQ(st.avg_frame_us_, 20000u);           // 20 ms = 20000 µs
    EXPECT_EQ(st.total_frames_, 1234u);
    EXPECT_EQ(st.gpu_submit_us_, 8500u);           // 8.5 ms
}

TEST(FramePerf, JankCountsFramesOver16_6ms) {
    std::vector<float> ms{5.0f, 17.0f, 16.0f, 33.0f, 16.6f, 16.7f};
    auto st = computeState(ms, 6, 0.0f);
    EXPECT_EQ(st.jank_count_, 3u);                 // 17, 33, 16.7 ( >16.6; 16.6 is NOT )
}

TEST(FramePerf, P99IgnoresASingleOutlier) {
    std::vector<float> ms;
    for (int i = 0; i < 99; ++i) ms.push_back(10.0f);
    ms.push_back(100.0f);                          // one outlier in 100 frames
    auto st = computeState(ms, 100, 0.0f);
    EXPECT_EQ(st.p99_frame_us_, 10000u);           // the lone spike is the top 1%, above p99
}

TEST(FramePerf, P99CatchesSustainedTail) {
    std::vector<float> ms;
    for (int i = 0; i < 95; ++i) ms.push_back(10.0f);
    for (int i = 0; i < 5;  ++i) ms.push_back(100.0f);   // worst 5% at 100 ms
    auto st = computeState(ms, 100, 0.0f);
    EXPECT_EQ(st.p99_frame_us_, 100000u);          // 99th pct now lands in the tail
}

TEST(FramePerf, RecentHoldsLast16NewestLast) {
    std::vector<float> ms;
    for (int i = 1; i <= 20; ++i) ms.push_back(static_cast<float>(i));  // 1..20 ms
    auto st = computeState(ms, 20, 0.0f);
    EXPECT_EQ(st.recent_[15], 20);                 // newest = 20 ms
    EXPECT_EQ(st.recent_[0], 5);                   // 16 back from newest = 5 ms
}

TEST(FramePerf, RecentClampsTo255AndPadsWhenShort) {
    std::vector<float> ms{300.0f, 7.0f};           // only 2 samples; 300→clamp 255
    auto st = computeState(ms, 2, 0.0f);
    EXPECT_EQ(st.recent_[15], 7);                  // newest
    EXPECT_EQ(st.recent_[14], 255);                // clamped
    EXPECT_EQ(st.recent_[0], 0);                   // padded (no sample)
}
