// Pure color-profile name/clamp mapping.

#include "xserver/color_profile.h"
#include <gtest/gtest.h>
#include <cstring>

using namespace fbx::xserver::colorprofile;

TEST(ColorProfile, Names) {
    EXPECT_STREQ(profileName(0), "sRGB");
    EXPECT_STREQ(profileName(1), "Display P3");
    EXPECT_STREQ(profileName(2), "Adobe RGB");
}

TEST(ColorProfile, ClampAndNameOutOfRange) {
    EXPECT_EQ(clampProfile(0), 0u);
    EXPECT_EQ(clampProfile(2), 2u);
    EXPECT_EQ(clampProfile(3), 0u);
    EXPECT_EQ(clampProfile(999), 0u);
    EXPECT_STREQ(profileName(99), "sRGB");   // unknown → sRGB
}
