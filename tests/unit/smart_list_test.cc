// Pure parser for SMART (kern.disks list + smartctl -H -A output).

#include "xserver/smart_list.h"
#include <gtest/gtest.h>
#include <cstdint>

using namespace fbx::xserver::smart;

TEST(SmartList, DiskListKeepsOnlyRealDisks) {
    auto d = parseDiskList("ada0 da0 cd0 nvd0 nda0 pass0 fd0 da1");
    ASSERT_EQ(d.size(), 5u);
    EXPECT_EQ(d[0], "ada0");
    EXPECT_EQ(d[1], "da0");
    EXPECT_EQ(d[2], "nvd0");
    EXPECT_EQ(d[3], "nda0");          // NVMe-via-CAM (modern FreeBSD)
    EXPECT_EQ(d[4], "da1");           // cd0/pass0/fd0 dropped
}

TEST(SmartList, DiskListEmpty) {
    EXPECT_TRUE(parseDiskList("").empty());
    EXPECT_TRUE(parseDiskList("cd0 fd0").empty());
}

TEST(SmartList, ParsesAtaHealthAndAttributes) {
    const char* out =
        "SMART overall-health self-assessment test result: PASSED\n"
        "\n"
        "ID# ATTRIBUTE_NAME          FLAG     VALUE WORST THRESH TYPE      UPDATED  WHEN_FAILED RAW_VALUE\n"
        "  5 Reallocated_Sector_Ct   0x0033   100   100   036    Pre-fail  Always       -       8\n"
        "  9 Power_On_Hours          0x0032   099   099   000    Old_age   Always       -       12345\n"
        "194 Temperature_Celsius     0x0022   059   045   000    Old_age   Always       -       41\n";
    SmartInfo d; d.name = "ada0";
    parseSmartctl(out, d);
    EXPECT_EQ(d.health, "PASSED");
    EXPECT_EQ(d.reallocated, 8u);
    EXPECT_EQ(d.power_on_hours, 12345u);
    EXPECT_EQ(d.temp_c, 41);
}

TEST(SmartList, ScsiHealthOkMapsToPassed) {
    SmartInfo d;
    parseSmartctl("SMART Health Status: OK\n", d);
    EXPECT_EQ(d.health, "PASSED");
}

TEST(SmartList, NvmeKeyValueLines) {
    const char* out =
        "SMART overall-health self-assessment test result: PASSED\n"
        "Temperature:                        40 Celsius\n"
        "Power On Hours:                     1,234\n";
    SmartInfo d;
    parseSmartctl(out, d);
    EXPECT_EQ(d.health, "PASSED");
    EXPECT_EQ(d.temp_c, 40);
    EXPECT_EQ(d.power_on_hours, 1234u);   // comma stripped
}

TEST(SmartList, UnknownWhenNoSmartctlOutput) {
    SmartInfo d; d.name = "ada0";
    parseSmartctl("", d);                 // smartctl absent / denied
    EXPECT_EQ(d.health, "N/A");
    EXPECT_EQ(d.temp_c, INT32_MIN);       // unknown sentinel preserved
    EXPECT_EQ(d.reallocated, 0u);
}
