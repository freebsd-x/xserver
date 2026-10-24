// Unit tests for pure Do-Not-Disturb expiry math.

#include "xserver/dnd_time.h"
#include <gtest/gtest.h>

using fbx::xserver::dnd::endsAt;

TEST(DndTime, UntilOffIsIndefinite) {
    EXPECT_EQ(endsAt(0, 1000000), 0u);
}

TEST(DndTime, OneHour) {
    time_t now = 1000000;
    EXPECT_EQ(endsAt(1, now), static_cast<uint32_t>(now + 3600));
}

TEST(DndTime, EightHours) {
    time_t now = 1000000;
    EXPECT_EQ(endsAt(2, now), static_cast<uint32_t>(now + 8 * 3600));
}

TEST(DndTime, UntilTomorrowIsFutureMidnight) {
    time_t now = 1000000;                 // some afternoon
    uint32_t e = endsAt(3, now);
    EXPECT_GT(e, static_cast<uint32_t>(now));          // in the future
    EXPECT_LE(e, static_cast<uint32_t>(now + 48 * 3600)); // within two days
}

TEST(DndTime, UnknownDurationIsIndefinite) {
    EXPECT_EQ(endsAt(99, 1000000), 0u);
}
