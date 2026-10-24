// Pure backlight(8) brightness parsing.

#include "xserver/backlight.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::backlight;

TEST(Backlight, ParsesBrightnessLine) {
    EXPECT_EQ(parseBrightness("brightness: 100\n"), 100);
    EXPECT_EQ(parseBrightness("brightness: 42\n"), 42);
    EXPECT_EQ(parseBrightness("brightness:0"), 0);          // no space
}

TEST(Backlight, ClampsAndRejects) {
    EXPECT_EQ(parseBrightness("brightness: 250"), 100);     // clamp
    EXPECT_EQ(parseBrightness(""), -1);                     // empty
    EXPECT_EQ(parseBrightness("no backlight devices"), -1); // absent
    EXPECT_EQ(parseBrightness("brightness: high"), -1);     // non-numeric
}
