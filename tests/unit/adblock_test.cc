// Pure date formatting for AdBlock's list_updated_ label.

#include "xserver/adblock.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::adblock;

TEST(AdBlock, FormatsEpochUtc) {
    EXPECT_EQ(formatListDate(0), "Jan 01 1970");
}

TEST(AdBlock, FormatsKnownDate) {
    // 2026-01-14 00:00:00 UTC = 1768348800.
    EXPECT_EQ(formatListDate(1768348800), "Jan 14 2026");
}

TEST(AdBlock, ProducesFixedWidthLabel) {
    // "%b %d %Y" is always 11 chars → fits list_updated_[24] comfortably.
    EXPECT_EQ(formatListDate(1768348800).size(), 11u);
}
