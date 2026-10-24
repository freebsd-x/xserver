// Pure CPU%→impact energy attribution.

#include "xserver/energy.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace fbx::xserver;
using namespace fbx::xserver::energy;
namespace shared = fbx::shared;

namespace {
shared::ProcessEntry mk(const char* name, uint32_t cpu_pct10) {
    shared::ProcessEntry e{};
    std::strncpy(e.name_, name, sizeof(e.name_) - 1);
    e.cpu_pct10_ = cpu_pct10;
    return e;
}
}

TEST(Energy, ImpactThresholds) {
    EXPECT_EQ(impactFor(60), 3u);   // very high
    EXPECT_EQ(impactFor(50), 3u);
    EXPECT_EQ(impactFor(20), 2u);   // high
    EXPECT_EQ(impactFor(5),  1u);   // medium
    EXPECT_EQ(impactFor(4),  0u);   // low
    EXPECT_EQ(impactFor(0),  0u);
}

TEST(Energy, RanksByCpuAndSetsFields) {
    // cpu_pct10: 550 (=55%) + 120 (=12%) + 0.
    auto u = computeUsage({mk("idle", 0), mk("render", 550), mk("sync", 120)});
    ASSERT_EQ(u.size(), 2u);                 // zero-CPU dropped
    EXPECT_STREQ(u[0].app_name_, "render");  // sorted desc
    EXPECT_EQ(u[0].cpu_avg_pct_, 55u);
    EXPECT_EQ(u[0].impact_, 3u);             // very high
    EXPECT_EQ(u[0].wakeups_sec_, 0u);        // unavailable
    EXPECT_EQ(u[1].cpu_avg_pct_, 12u);
    EXPECT_EQ(u[1].impact_, 1u);             // medium
}

TEST(Energy, CpuClampsTo100) {
    auto u = computeUsage({mk("spin", 2500)});   // 250% (multi-core) → clamp 100
    ASSERT_EQ(u.size(), 1u);
    EXPECT_EQ(u[0].cpu_avg_pct_, 100u);
    EXPECT_EQ(u[0].impact_, 3u);
}

TEST(Energy, EmptyAndAllIdle) {
    EXPECT_TRUE(computeUsage({}).empty());
    EXPECT_TRUE(computeUsage({mk("a", 0)}).empty());
}

TEST(Energy, Paging) {
    std::vector<shared::EnergyEntry> all(5);
    EXPECT_EQ(buildPage(all, 0).count_, 3u);
    EXPECT_EQ(buildPage(all, 0).has_more_, 1u);
    EXPECT_EQ(buildPage(all, 1).count_, 2u);
    EXPECT_EQ(buildPage(all, 1).has_more_, 0u);
}
