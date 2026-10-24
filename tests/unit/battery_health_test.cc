// Pure parser for `acpiconf -i 0` battery wear fields.

#include "xserver/battery_health.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::battery;

namespace {
constexpr const char* kAcpiconf =
    "Design capacity:\t50086 mWh\n"
    "Last full capacity:\t39153 mWh\n"
    "Technology:\t\tsecondary (rechargeable)\n"
    "Design voltage:\t\t11897 mV\n"
    "Capacity (warn):\t5008 mWh\n"
    "Cycle Count:\t\t21\n"
    "Model number:\t\tASUS Battery\n";
}

TEST(BatteryHealth, ParsesCapacitiesAndCycleCount) {
    auto b = parseAcpiconf(kAcpiconf);
    EXPECT_EQ(b.design_mwh, 50086u);
    EXPECT_EQ(b.last_full_mwh, 39153u);
    EXPECT_EQ(b.cycle_count, 21u);
}

TEST(BatteryHealth, HealthPercentFromCapacities) {
    auto b = parseAcpiconf(kAcpiconf);
    EXPECT_EQ(healthPct(b), 78u);         // 39153 / 50086 = 78%
}

TEST(BatteryHealth, DistinguishesDesignVoltageFromCapacity) {
    // "Design voltage" must NOT be mistaken for "Design capacity".
    auto b = parseAcpiconf("Design voltage:\t\t11897 mV\n");
    EXPECT_EQ(b.design_mwh, 0u);
}

TEST(BatteryHealth, NoBatteryIsZeroHealth) {
    auto b = parseAcpiconf("");           // acpiconf found no battery
    EXPECT_EQ(b.design_mwh, 0u);
    EXPECT_EQ(healthPct(b), 0u);          // avoids divide-by-zero
}

TEST(BatteryHealth, HealthClampsTo100) {
    BatteryHealth b{100u, 120u, 3u};      // last-full > design (freshly calibrated)
    EXPECT_EQ(healthPct(b), 100u);
}
