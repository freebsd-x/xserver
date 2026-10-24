// Pure parser for kldstat(8) output.

#include "xserver/kmod_list.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::kmod;

namespace {
constexpr const char* kKldstat =
    "Id Refs Address                 Size Name\n"
    " 1   33 0xffffffff80200000  1f73478 kernel\n"
    " 2    1 0xffffffff82174000     3f90 ichsmb.ko\n"
    " 3    1 0xffffffff82178000      a20 smbus.ko\n";
}

TEST(KmodList, ParsesIdRefsSizeName) {
    auto m = parseKldstat(kKldstat);
    ASSERT_EQ(m.size(), 3u);
    EXPECT_EQ(m[0].name, "kernel");
    EXPECT_EQ(m[0].refs, 33u);
    EXPECT_EQ(m[0].size_kb, 0x1f73478u / 1024u);   // bare-hex Size → KB
    EXPECT_EQ(m[1].name, "ichsmb.ko");
    EXPECT_EQ(m[1].refs, 1u);
    EXPECT_EQ(m[1].size_kb, 0x3f90u / 1024u);
    EXPECT_EQ(m[2].name, "smbus.ko");
}

TEST(KmodList, SkipsHeaderAndBlankLines) {
    EXPECT_TRUE(parseKldstat("Id Refs Address                 Size Name\n").empty());
    EXPECT_TRUE(parseKldstat("").empty());
    EXPECT_TRUE(parseKldstat("\n\n").empty());
}

TEST(KmodList, IgnoresMalformedLines) {
    // Missing the 0x address column → not a data row.
    EXPECT_TRUE(parseKldstat("garbage line without columns\n").empty());
    EXPECT_TRUE(parseKldstat(" 1 2 3 4\n").empty());   // only 4 tokens, no 0x addr
}
