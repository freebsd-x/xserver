// Quiet hours, breakthrough, per-app sound map and tone synthesis.

#include "xserver/notif_parse.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::notif;

namespace {
std::tm at(int wday, int hour, int min) {
    std::tm t{};
    t.tm_wday = wday;   // 0 = Sunday
    t.tm_hour = hour;
    t.tm_min  = min;
    return t;
}
}  // namespace

TEST(QuietHours, OvernightWindow) {
    Schedule s;
    s.enabled = true;               // 22:00 → 07:00
    EXPECT_TRUE(inQuietWindow(s, at(3, 23, 0)));
    EXPECT_TRUE(inQuietWindow(s, at(3, 6, 59)));
    EXPECT_FALSE(inQuietWindow(s, at(3, 7, 0)));    // end is exclusive
    EXPECT_FALSE(inQuietWindow(s, at(3, 12, 0)));
    EXPECT_TRUE(inQuietWindow(s, at(3, 22, 0)));
    s.enabled = false;
    EXPECT_FALSE(inQuietWindow(s, at(3, 23, 0)));
}

TEST(QuietHours, SameDayWindowAndEmptyWindow) {
    Schedule s;
    s.enabled = true;
    s.start_min = 13 * 60;
    s.end_min   = 14 * 60 + 30;
    EXPECT_TRUE(inQuietWindow(s, at(2, 13, 45)));
    EXPECT_FALSE(inQuietWindow(s, at(2, 14, 30)));
    EXPECT_FALSE(inQuietWindow(s, at(2, 12, 59)));
    s.end_min = s.start_min;                        // start == end: no window
    EXPECT_FALSE(inQuietWindow(s, at(2, 13, 0)));
}

TEST(QuietHours, WeekendOnlyFollowsTheStartDay) {
    Schedule s;
    s.enabled = true;
    s.weekend_only = true;          // 22:00 → 07:00
    EXPECT_TRUE(inQuietWindow(s, at(6, 23, 0)));    // Saturday night
    EXPECT_TRUE(inQuietWindow(s, at(0, 3, 0)));     // ...into Sunday morning
    EXPECT_TRUE(inQuietWindow(s, at(1, 3, 0)));     // Sunday night → Monday 03:00
    EXPECT_FALSE(inQuietWindow(s, at(1, 23, 0)));   // Monday night
    EXPECT_FALSE(inQuietWindow(s, at(6, 3, 0)));    // Friday night → Saturday 03:00
}

TEST(QuietHours, Breakthrough) {
    Schedule s;
    EXPECT_TRUE(breaksThrough(s, "Clock"));
    EXPECT_TRUE(breaksThrough(s, "Calendar"));
    EXPECT_TRUE(breaksThrough(s, "Phone"));
    EXPECT_FALSE(breaksThrough(s, "Mail"));
    EXPECT_FALSE(breaksThrough(s, ""));
    s.allow_alarms = false;
    EXPECT_FALSE(breaksThrough(s, "Clock"));
    s.allow_calls = false;
    EXPECT_FALSE(breaksThrough(s, "Phone"));
}

TEST(NotifSounds, MapRoundTripAndValidation) {
    auto m = parseSoundMap("Mail=1;Calendar=3;bad;x=9;Weather=0;;a;b=2");
    EXPECT_EQ(m.size(), 4u);
    EXPECT_EQ(m.at("Mail"), kChime);
    EXPECT_EQ(m.at("Calendar"), kSilent);
    EXPECT_EQ(m.at("b"), kPing);                     // "a;b=2" splits into "a" (skipped) and "b=2"
    EXPECT_EQ(m.count("x"), 0u);                     // sound id out of range
    EXPECT_EQ(parseSoundMap(formatSoundMap(m)), m);
    EXPECT_FALSE(validAppName("a=b"));
    EXPECT_FALSE(validAppName(""));
    EXPECT_TRUE(validAppName("Weather"));
}

TEST(NotifSounds, SynthesisLengthsAndVolume) {
    EXPECT_TRUE(synthesize(kSilent, 100).empty());
    auto chime = synthesize(kChime, 100);
    auto ping  = synthesize(kPing, 100);
    EXPECT_NEAR(static_cast<double>(chime.size()), 0.42 * 48000, 2);   // two overlapping notes
    EXPECT_NEAR(static_cast<double>(ping.size()), 0.22 * 48000, 2);
    auto peak = [](const std::vector<int16_t>& v) {
        int p = 0;
        for (int16_t s : v) p = std::max(p, std::abs(static_cast<int>(s)));
        return p;
    };
    EXPECT_GT(peak(ping), 5000);
    EXPECT_LE(peak(chime), 32767);
    EXPECT_EQ(peak(synthesize(kPing, 0)), 0);                       // volume 0 is silent
    EXPECT_LT(peak(synthesize(kPing, 20)), peak(ping));
}
