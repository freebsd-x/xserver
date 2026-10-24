// Pure pfctl status parsing.

#include "xserver/firewall.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::firewall;

TEST(Firewall, ParsesEnabledStatus) {
    EXPECT_TRUE(parseEnabled("Status: Enabled for 0 days 01:23:45\n  Debug: Urgent\n"));
    EXPECT_FALSE(parseEnabled("Status: Disabled\n"));
    EXPECT_FALSE(parseEnabled(""));                       // unprivileged → empty
}

TEST(Firewall, CountsRuleLines) {
    EXPECT_EQ(countRules("block drop in all\npass out all\n"), 2u);
    EXPECT_EQ(countRules(""), 0u);
    EXPECT_EQ(countRules("\n  \n\t\n"), 0u);              // blank lines don't count
    EXPECT_EQ(countRules("pass in quick on lo0 all"), 1u); // no trailing newline
}
