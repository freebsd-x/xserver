// Pure helpers for com.freebsdx.InputDevice.

#include "xserver/input_devices.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::inputdev;

TEST(InputDevices, IdIsStableAndNonZero) {
    EXPECT_EQ(deviceId("SynPS/2 Synaptics TouchPad"), deviceId("SynPS/2 Synaptics TouchPad"));
    EXPECT_NE(deviceId("SynPS/2 Synaptics TouchPad"), deviceId("AT keyboard"));
    EXPECT_NE(deviceId(""), 0u);
}

TEST(InputDevices, MostSpecificTypeWins) {
    EXPECT_EQ(pickType(true, false, false, false), kKeyboard);
    EXPECT_EQ(pickType(true, true, false, false), kPointer);   // keyboard+mouse combo
    EXPECT_EQ(pickType(false, true, true, false), kTouch);
    EXPECT_EQ(pickType(false, true, false, true), kTablet);
}

TEST(InputDevices, SensitivityMapsToLibinputRange) {
    EXPECT_DOUBLE_EQ(sensitivityToSpeed(0), -1.0);
    EXPECT_DOUBLE_EQ(sensitivityToSpeed(50), 0.0);
    EXPECT_DOUBLE_EQ(sensitivityToSpeed(100), 1.0);
    EXPECT_DOUBLE_EQ(sensitivityToSpeed(250), 1.0);             // clamped
    for (uint32_t s : {0u, 13u, 50u, 77u, 100u})
        EXPECT_EQ(speedToSensitivity(sensitivityToSpeed(s)), s);
    EXPECT_EQ(speedToSensitivity(-3.0), 0u);
}

TEST(InputDevices, OverridesRoundTripAndRejectJunk) {
    std::map<uint32_t, Override> m{{42, {30, false}}, {4000000000u, {100, true}}};
    EXPECT_EQ(parseOverrides(formatOverrides(m)).size(), 2u);
    auto back = parseOverrides(formatOverrides(m));
    EXPECT_EQ(back[42].sensitivity, 30u);
    EXPECT_FALSE(back[42].adaptive);
    EXPECT_TRUE(back[4000000000u].adaptive);

    auto junk = parseOverrides("0:10:1;7:101:1;8:5:2;x:1:1;9:20;12:40:1;;99999999999:1:1");
    ASSERT_EQ(junk.size(), 1u);                                  // only 12:40:1 survives
    EXPECT_EQ(junk[12].sensitivity, 40u);
    EXPECT_TRUE(parseOverrides("").empty());
}
