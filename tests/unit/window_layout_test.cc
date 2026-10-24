// Unit tests for the pure master/stack tiling geometry (window_layout.h).

#include "xserver/window_layout.h"
#include <gtest/gtest.h>

using fbx::xserver::layout::masterStack;
using fbx::xserver::layout::TileRect;

namespace {
constexpr float kEps = 0.01f;
bool near(float a, float b) { return std::abs(a - b) < kEps; }
}

TEST(WindowLayoutTest, EmptyYieldsNothing) {
    EXPECT_TRUE(masterStack(0, 0, 0, 1920, 1080).empty());
    EXPECT_TRUE(masterStack(3, 0, 0, 0, 0).empty());   // degenerate output
}

TEST(WindowLayoutTest, SingleWindowFillsOutput) {
    auto r = masterStack(1, 0, 0, 1920, 1080);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_TRUE(near(r[0].x, 0));
    EXPECT_TRUE(near(r[0].y, 0));
    EXPECT_TRUE(near(r[0].w, 1920));
    EXPECT_TRUE(near(r[0].h, 1080));
}

TEST(WindowLayoutTest, TwoWindowsMasterStackSplit) {
    // master_ratio 0.6 → master 1152 wide, stack 768 wide, both full height.
    auto r = masterStack(2, 0, 0, 1920, 1080, 0.6f, 1);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_TRUE(near(r[0].w, 1152));           // master
    EXPECT_TRUE(near(r[0].h, 1080));
    EXPECT_TRUE(near(r[1].x, 1152));           // stack starts after master
    EXPECT_TRUE(near(r[1].w, 768));
    EXPECT_TRUE(near(r[1].h, 1080));
}

TEST(WindowLayoutTest, ThreeWindowsStackSplitByHeight) {
    auto r = masterStack(3, 0, 0, 1000, 900, 0.5f, 1);
    ASSERT_EQ(r.size(), 3u);
    // master: 500 wide full height
    EXPECT_TRUE(near(r[0].w, 500));
    EXPECT_TRUE(near(r[0].h, 900));
    // two stacked: 500 wide, 450 tall each, stacked vertically
    EXPECT_TRUE(near(r[1].x, 500));
    EXPECT_TRUE(near(r[1].y, 0));
    EXPECT_TRUE(near(r[1].h, 450));
    EXPECT_TRUE(near(r[2].y, 450));
    EXPECT_TRUE(near(r[2].h, 450));
}

TEST(WindowLayoutTest, TilesCoverOutputWithoutOverlap) {
    const float OW = 1600, OH = 1200;
    auto r = masterStack(5, 0, 0, OW, OH, 0.55f, 1);
    ASSERT_EQ(r.size(), 5u);
    // master right edge == stack left edge (no gap/overlap horizontally)
    EXPECT_TRUE(near(r[0].x + r[0].w, r[1].x));
    // stack rows are contiguous and sum to full height
    float sum = 0;
    for (size_t i = 1; i < r.size(); ++i) sum += r[i].h;
    EXPECT_TRUE(near(sum, OH));
    // consecutive stack rows are adjacent
    for (size_t i = 2; i < r.size(); ++i)
        EXPECT_TRUE(near(r[i-1].y + r[i-1].h, r[i].y));
}

TEST(WindowLayoutTest, MasterCountTwo) {
    // 4 windows, 2 masters → master column has 2 rows, stack has 2 rows.
    auto r = masterStack(4, 0, 0, 1000, 800, 0.6f, 2);
    ASSERT_EQ(r.size(), 4u);
    EXPECT_TRUE(near(r[0].w, 600));  EXPECT_TRUE(near(r[0].h, 400));  // master row 0
    EXPECT_TRUE(near(r[1].y, 400));  EXPECT_TRUE(near(r[1].w, 600));  // master row 1
    EXPECT_TRUE(near(r[2].x, 600));  EXPECT_TRUE(near(r[2].w, 400));  // stack row 0
    EXPECT_TRUE(near(r[3].y, 400));                                    // stack row 1
}

TEST(WindowLayoutTest, AllInMasterWhenCountCovers) {
    // n <= master_count → single full-width column.
    auto r = masterStack(2, 0, 0, 1000, 600, 0.6f, 3);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_TRUE(near(r[0].w, 1000));
    EXPECT_TRUE(near(r[1].w, 1000));
    EXPECT_TRUE(near(r[0].h, 300));
    EXPECT_TRUE(near(r[1].h, 300));
}

TEST(WindowLayoutTest, RatioClamped) {
    // Absurd ratios clamp into [0.1, 0.9].
    auto lo = masterStack(2, 0, 0, 1000, 500, 0.0f, 1);
    EXPECT_TRUE(near(lo[0].w, 100));   // clamped to 0.1
    auto hi = masterStack(2, 0, 0, 1000, 500, 5.0f, 1);
    EXPECT_TRUE(near(hi[0].w, 900));   // clamped to 0.9
}

TEST(WindowLayoutTest, OffsetOutputRespected) {
    // Second monitor at x=1920.
    auto r = masterStack(2, 1920, 28, 1920, 1052, 0.5f, 1);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_TRUE(near(r[0].x, 1920));
    EXPECT_TRUE(near(r[0].y, 28));
    EXPECT_TRUE(near(r[1].x, 1920 + 960));
}

// ── directional focus ─────────────────────────────────────────────────────────
using fbx::xserver::layout::pickDirectional;
using fbx::xserver::layout::WinCenter;
using fbx::xserver::layout::Dir;

namespace {
// master (id 1) left, two stacked on the right (id 2 top, id 3 bottom).
std::vector<WinCenter> threeUp() {
    return { {1, 250, 300}, {2, 750, 150}, {3, 750, 450} };
}
}

TEST(DirectionalFocusTest, RightFromMasterPicksAlignedStack) {
    // From master (y=300), Right → the stack window closest in y (id 2 @150 is
    // 150 off; id 3 @450 is 150 off — tie broken by first-seen id 2).
    EXPECT_EQ(pickDirectional(threeUp(), 1, Dir::Right), 2u);
}

TEST(DirectionalFocusTest, LeftFromStackReturnsMaster) {
    EXPECT_EQ(pickDirectional(threeUp(), 2, Dir::Left), 1u);
    EXPECT_EQ(pickDirectional(threeUp(), 3, Dir::Left), 1u);
}

TEST(DirectionalFocusTest, DownWithinStack) {
    EXPECT_EQ(pickDirectional(threeUp(), 2, Dir::Down), 3u);
}

TEST(DirectionalFocusTest, UpWithinStack) {
    EXPECT_EQ(pickDirectional(threeUp(), 3, Dir::Up), 2u);
}

TEST(DirectionalFocusTest, NoCandidateReturnsSelf) {
    // Nothing to the left of the master.
    EXPECT_EQ(pickDirectional(threeUp(), 1, Dir::Left), 1u);
    // Master has nothing above/below aligned strictly? id below/above only in stack col.
    EXPECT_EQ(pickDirectional(threeUp(), 3, Dir::Down), 3u);
}

TEST(DirectionalFocusTest, UnknownCurrentReturnsSelf) {
    EXPECT_EQ(pickDirectional(threeUp(), 999, Dir::Right), 999u);
}

TEST(DirectionalFocusTest, PrefersAlignedOverCloser) {
    // A window slightly closer on the primary axis but far off-axis should lose
    // to one a bit further but well-aligned.
    std::vector<WinCenter> w = {
        {1, 0, 0},
        {2, 100, 400},   // primary 100, perp 400 → score 100+800=900
        {3, 200, 10},    // primary 200, perp 10  → score 200+20 =220
    };
    EXPECT_EQ(pickDirectional(w, 1, Dir::Right), 3u);
}
