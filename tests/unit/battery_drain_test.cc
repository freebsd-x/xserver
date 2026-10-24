// Pure CPU-share energy attribution for BatteryDrain.

#include "xserver/battery_drain.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace fbx::xserver;
using namespace fbx::xserver::batdrain;
namespace shared = fbx::shared;

namespace {
shared::ProcessEntry mk(const char* name, uint32_t pid, uint32_t cpu_pct10) {
    shared::ProcessEntry e{};
    std::strncpy(e.name_, name, sizeof(e.name_) - 1);
    e.pid_ = pid;
    e.cpu_pct10_ = cpu_pct10;
    return e;
}
}

TEST(BatteryDrain, RanksByCpuShareAndSumsTo100) {
    // CPU% ×10: 600 + 300 + 100 = 1000 total → 60% / 30% / 10%.
    auto d = computeDrain({mk("idle", 1, 100), mk("browser", 2, 600), mk("editor", 3, 300)});
    ASSERT_EQ(d.size(), 3u);
    EXPECT_STREQ(d[0].app_name_, "browser");   // sorted desc
    EXPECT_EQ(d[0].energy_pct_, 60u);
    EXPECT_EQ(d[0].pid_, 2u);
    EXPECT_EQ(d[1].energy_pct_, 30u);
    EXPECT_EQ(d[2].energy_pct_, 10u);
    EXPECT_EQ(d[0].energy_pct_ + d[1].energy_pct_ + d[2].energy_pct_, 100u);
}

TEST(BatteryDrain, ImpactThresholds) {
    EXPECT_EQ(impactFor(25), static_cast<uint32_t>(shared::BatteryDrainImpact::High));
    EXPECT_EQ(impactFor(20), static_cast<uint32_t>(shared::BatteryDrainImpact::High));
    EXPECT_EQ(impactFor(10), static_cast<uint32_t>(shared::BatteryDrainImpact::Medium));
    EXPECT_EQ(impactFor(5),  static_cast<uint32_t>(shared::BatteryDrainImpact::Medium));
    EXPECT_EQ(impactFor(4),  static_cast<uint32_t>(shared::BatteryDrainImpact::Low));
    EXPECT_EQ(impactFor(0),  static_cast<uint32_t>(shared::BatteryDrainImpact::Low));
}

TEST(BatteryDrain, DropsZeroCpuProcesses) {
    auto d = computeDrain({mk("busy", 1, 500), mk("asleep", 2, 0), mk("idle", 3, 0)});
    ASSERT_EQ(d.size(), 1u);                   // only the busy one has measurable drain
    EXPECT_STREQ(d[0].app_name_, "busy");
    EXPECT_EQ(d[0].energy_pct_, 100u);         // sole consumer → 100%
}

TEST(BatteryDrain, EmptyAndAllIdle) {
    EXPECT_TRUE(computeDrain({}).empty());
    EXPECT_TRUE(computeDrain({mk("a", 1, 0), mk("b", 2, 0)}).empty());
}

TEST(BatteryDrain, Paging) {
    std::vector<shared::BatteryDrainEntry> all(7);
    auto p0 = buildPage(all, 0);
    EXPECT_EQ(p0.total_, 7u);
    EXPECT_EQ(p0.count_, 3u);
    EXPECT_EQ(p0.has_more_, 1u);
    auto p2 = buildPage(all, 2);
    EXPECT_EQ(p2.count_, 1u);                   // 7 = 3 + 3 + 1
    EXPECT_EQ(p2.has_more_, 0u);
}
