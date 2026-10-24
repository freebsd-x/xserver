// Pure precision clamping for the Location per-app policy.

#include "xserver/location.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::location;

TEST(Location, ClampPrecisionKeepsValidValues) {
    EXPECT_EQ(clampPrecision(0), 0u);   // off
    EXPECT_EQ(clampPrecision(1), 1u);   // approximate
    EXPECT_EQ(clampPrecision(2), 2u);   // precise
}

TEST(Location, ClampPrecisionRejectsOutOfRange) {
    EXPECT_EQ(clampPrecision(3), 0u);
    EXPECT_EQ(clampPrecision(99), 0u);
    EXPECT_EQ(clampPrecision(0xFFFFFFFFu), 0u);
}
