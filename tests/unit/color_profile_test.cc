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

TEST(ColorProfileMatrix, SrgbIsIdentityAndWhiteStaysWhite) {
    auto id = fbx::xserver::colorprofile::matrixFor(0);
    EXPECT_EQ(id, (std::array<double, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1}));
    for (uint32_t p : {1u, 2u, 7u}) {
        auto m = fbx::xserver::colorprofile::matrixFor(p);
        for (int r = 0; r < 3; ++r)
            EXPECT_NEAR(m[r * 3] + m[r * 3 + 1] + m[r * 3 + 2], 1.0, 0.002) << "profile " << p << " row " << r;
    }
    // On a wider-gamut display, sRGB red maps to a less saturated red.
    auto p3 = fbx::xserver::colorprofile::matrixFor(1);
    EXPECT_LT(p3[0], 1.0);
    EXPECT_GT(p3[3], 0.0);
    EXPECT_EQ(fbx::xserver::colorprofile::matrixFor(99), id);   // unknown → sRGB
}
