// Unit tests for the NMEA 0183 parser used by the GPS service.
#include "xserver/nmea_parse.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::nmea;

namespace {
bool neard(double a, double b, double eps = 1e-4) { return std::abs(a - b) < eps; }
}

TEST(Nmea, ParsesGgaFix) {
    // Textbook GGA (checksum *47 is correct for this body).
    // 4807.038,N → 48 + 7.038/60 = 48.1173; 01131.000,E → 11 + 31/60 = 11.5167
    Fix f = parseSentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");
    ASSERT_TRUE(f.valid);
    EXPECT_TRUE(neard(f.lat, 48.1173)) << "lat=" << f.lat;
    EXPECT_TRUE(neard(f.lon, 11.5167)) << "lon=" << f.lon;
    EXPECT_GT(f.accuracy_m, 0.0f);          // HDOP 0.9 → ~4.5m
    EXPECT_LT(f.accuracy_m, 20.0f);
}

TEST(Nmea, ParsesRmcActive) {
    // Textbook RMC (checksum *6A is correct for this body).
    Fix f = parseSentence("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A");
    ASSERT_TRUE(f.valid);
    EXPECT_TRUE(neard(f.lat, 48.1173));
    EXPECT_TRUE(neard(f.lon, 11.5167));
}

TEST(Nmea, SouthAndWestAreNegative) {
    // No checksum supplied → accepted; sign comes from the S/E hemispheres.
    Fix f = parseSentence("$GPGGA,000000,3352.000,S,15112.000,E,1,05,1.2,0,M,,M,,");
    ASSERT_TRUE(f.valid);
    EXPECT_LT(f.lat, 0.0) << "S latitude must be negative";
    EXPECT_GT(f.lon, 0.0) << "E longitude must be positive";
    // 3352.000 S → -(33 + 52/60) = -33.8667
    EXPECT_TRUE(neard(f.lat, -33.8667)) << "lat=" << f.lat;
}

TEST(Nmea, NoFixQualityZeroRejected) {
    // Quality field = 0 → receiver has no fix (no checksum so only quality decides).
    Fix f = parseSentence("$GPGGA,123519,4807.038,N,01131.000,E,0,00,,,,M,,M,,");
    EXPECT_FALSE(f.valid);
}

TEST(Nmea, RmcVoidRejected) {
    Fix f = parseSentence("$GPRMC,123519,V,4807.038,N,01131.000,E,,,230394,,");
    EXPECT_FALSE(f.valid);
}

TEST(Nmea, TalkerIdIgnored) {
    // GN prefix (multi-constellation) must parse the same as GP.
    Fix f = parseSentence("$GNGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    EXPECT_TRUE(f.valid);
}

TEST(Nmea, BadChecksumRejected) {
    // Same body as the valid GGA but with a deliberately wrong checksum (*00).
    Fix f = parseSentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00");
    EXPECT_FALSE(f.valid) << "checksum mismatch must be rejected";
}

TEST(Nmea, MissingChecksumAccepted) {
    Fix f = parseSentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    EXPECT_TRUE(f.valid);
}

TEST(Nmea, NonPositionalSentenceIgnored) {
    // GSV = satellites in view, no position (no checksum → rejected purely by type).
    Fix f = parseSentence("$GPGSV,3,1,11,03,03,111,00,04,15,270,00");
    EXPECT_FALSE(f.valid);
}

TEST(Nmea, GarbageRejected) {
    EXPECT_FALSE(parseSentence("").valid);
    EXPECT_FALSE(parseSentence("hello world").valid);
    EXPECT_FALSE(parseSentence("$GP").valid);
    EXPECT_FALSE(parseSentence("$GPGGA").valid);
}

TEST(Nmea, StreamReturnsLatestValidFix) {
    // Three lines; the last valid fix (Sydney, southern hemisphere) should win.
    std::string buf =
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\n"
        "$GPRMC,123519,V,,,,,,,230394,,\n"
        "$GPGGA,123520,3352.000,S,15112.000,E,1,05,1.2,0,M,,M,,\n";
    Fix f = parseStream(buf);
    ASSERT_TRUE(f.valid);
    EXPECT_LT(f.lat, 0.0) << "latest fix (Sydney, S) should win; lat=" << f.lat;
}
