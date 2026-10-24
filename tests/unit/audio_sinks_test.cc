// Pure parser for /dev/sndstat audio output sinks.

#include "xserver/audio_sinks.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::audio;

namespace {
constexpr const char* kSndstat =
    "Installed devices:\n"
    "pcm0: <Realtek ALC294 (Analog 2.0+HP/4ch)> (play/rec) default\n"
    "pcm1: <Intel Kaby Lake (HDMI/DP 8ch)> (play)\n"
    "No devices installed from userspace.\n";
}

TEST(AudioSinks, ParsesPlayDevicesWithActiveMarker) {
    auto s = parseSndstat(kSndstat);
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(s[0].id, "pcm0");
    EXPECT_EQ(s[0].name, "Realtek ALC294 (Analog 2.0+HP/4ch)");  // inner parens kept
    EXPECT_TRUE(s[0].active);                                    // "default"
    EXPECT_EQ(s[1].id, "pcm1");
    EXPECT_EQ(s[1].name, "Intel Kaby Lake (HDMI/DP 8ch)");
    EXPECT_FALSE(s[1].active);
}

TEST(AudioSinks, ExcludesRecordOnlyAndNonDeviceLines) {
    auto s = parseSndstat(
        "Installed devices:\n"
        "pcm2: <USB Microphone> (rec)\n"            // no play → not a sink
        "No devices installed from userspace.\n");
    EXPECT_TRUE(s.empty());
}

TEST(AudioSinks, EmptyInput) {
    EXPECT_TRUE(parseSndstat("").empty());
}

TEST(AudioSinks, UnitOfId) {
    EXPECT_EQ(unitOfId("pcm0"), 0u);
    EXPECT_EQ(unitOfId("pcm7"), 7u);
    EXPECT_EQ(unitOfId("pcm13"), 13u);
    EXPECT_EQ(unitOfId("bogus"), UINT32_MAX);
    EXPECT_EQ(unitOfId("pcm"), UINT32_MAX);        // no digits
}
