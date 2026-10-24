// Unit tests for the pure `pkg upgrade -n` parser.

#include "xserver/pkg_parse.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::pkg;

TEST(PkgParse, TabIndentedUpgradeLine) {
    Upgrade u;
    ASSERT_TRUE(parseLine("\tcurl: 8.1.0 -> 8.2.0", u));
    EXPECT_EQ(u.name, "curl");
    EXPECT_EQ(u.old_ver, "8.1.0");
    EXPECT_EQ(u.new_ver, "8.2.0");
}

TEST(PkgParse, TrailingAnnotationStripped) {
    Upgrade u;
    ASSERT_TRUE(parseLine("\tgit: 2.40.0 -> 2.41.0 [FreeBSD]", u));
    EXPECT_EQ(u.name, "git");
    EXPECT_EQ(u.new_ver, "2.41.0");   // annotation dropped
}

TEST(PkgParse, NonUpgradeLinesRejected) {
    Upgrade u;
    EXPECT_FALSE(parseLine("Updating FreeBSD repository catalogue...", u));
    EXPECT_FALSE(parseLine("Number of packages to be upgraded: 2", u));
    EXPECT_FALSE(parseLine("", u));
    EXPECT_FALSE(parseLine("newpkg: (new) [reason]", u));   // no " -> "
}

TEST(PkgParse, ParsesFullOutputBlock) {
    const char* out =
        "Updating FreeBSD repository catalogue...\n"
        "The following 2 package(s) will be affected:\n"
        "\n"
        "Installed packages to be UPGRADED:\n"
        "\tcurl: 8.1.0 -> 8.2.0\n"
        "\tgit: 2.40.0 -> 2.41.0\n"
        "\n"
        "Number of packages to be upgraded: 2\n";
    auto ups = parseUpgrades(out);
    ASSERT_EQ(ups.size(), 2u);
    EXPECT_EQ(ups[0].name, "curl");
    EXPECT_EQ(ups[1].name, "git");
    EXPECT_EQ(ups[1].old_ver, "2.40.0");
}

TEST(PkgParse, UpToDateYieldsNothing) {
    EXPECT_TRUE(parseUpgrades("Your packages are up to date.\n").empty());
}
