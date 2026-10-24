// Pure XKB layout/variant validation.

#include "xserver/keymap.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::keymap;

TEST(Keymap, ValidLayouts) {
    EXPECT_TRUE(isValidLayout("us"));
    EXPECT_TRUE(isValidLayout("fr"));
    EXPECT_TRUE(isValidLayout("latam"));
}

TEST(Keymap, RejectsBadLayouts) {
    EXPECT_FALSE(isValidLayout(""));                 // empty
    EXPECT_FALSE(isValidLayout("US"));               // uppercase
    EXPECT_FALSE(isValidLayout("us2"));              // digit
    EXPECT_FALSE(isValidLayout("us;rm -rf"));        // injection chars
    EXPECT_FALSE(isValidLayout("abcdefghijklmnop"));  // 16 > 15
}

TEST(Keymap, Variants) {
    EXPECT_TRUE(isValidVariant(""));                 // no variant is valid
    EXPECT_TRUE(isValidVariant("dvorak"));
    EXPECT_TRUE(isValidVariant("nodeadkeys"));
    EXPECT_FALSE(isValidVariant("Dvorak"));          // uppercase
    EXPECT_FALSE(isValidVariant("intl3"));           // digit
}
