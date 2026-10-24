// Pure page-builder for DisplayArrange.

#include "xserver/display_arrange.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::disparrange;

TEST(DisplayArrange, EmptyLayoutIsInvalidPage) {
    auto pg = buildPage({}, 0);
    EXPECT_EQ(pg.total_, 0u);
    EXPECT_EQ(pg.valid_, 0u);
    EXPECT_EQ(pg.has_more_, 0u);
}

TEST(DisplayArrange, SingleOutputPrimary) {
    ArrangeOutput o; o.width = 1920; o.height = 1080; o.primary = true; o.name = "output-0";
    auto pg = buildPage({o}, 0);
    EXPECT_EQ(pg.total_, 1u);
    EXPECT_EQ(pg.valid_, 1u);
    EXPECT_EQ(pg.output_id_, 0u);
    EXPECT_EQ(pg.is_primary_, 1u);
    EXPECT_EQ(pg.mirroring_, 0u);
    EXPECT_EQ(pg.width_, 1920u);
    EXPECT_EQ(pg.height_, 1080u);
    EXPECT_STREQ(pg.name_, "output-0");
    EXPECT_EQ(pg.has_more_, 0u);
}

TEST(DisplayArrange, PagesOnePerOutput) {
    ArrangeOutput a; a.width = 2560; a.height = 1440; a.primary = true;  a.name = "output-0";
    ArrangeOutput b; b.width = 1920; b.height = 1080; b.mirroring = true; b.name = "output-1";
    std::vector<ArrangeOutput> outs{a, b};

    auto p0 = buildPage(outs, 0);
    EXPECT_EQ(p0.total_, 2u);
    EXPECT_EQ(p0.valid_, 1u);
    EXPECT_EQ(p0.is_primary_, 1u);
    EXPECT_EQ(p0.has_more_, 1u);          // one more follows

    auto p1 = buildPage(outs, 1);
    EXPECT_EQ(p1.valid_, 1u);
    EXPECT_EQ(p1.output_id_, 1u);
    EXPECT_EQ(p1.mirroring_, 1u);
    EXPECT_EQ(p1.width_, 1920u);
    EXPECT_EQ(p1.has_more_, 0u);

    auto p2 = buildPage(outs, 2);         // past the end
    EXPECT_EQ(p2.valid_, 0u);
}
