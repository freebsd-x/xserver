// Unit tests for pure login-session helpers.

#include "xserver/session_info.h"
#include <gtest/gtest.h>
#include <string>

using namespace fbx::xserver::session;

// ── classifyType ──────────────────────────────────────────────────────────────

TEST(SessionInfo, RemoteHostIsSsh) {
    EXPECT_STREQ(classifyType("pts/3", "10.0.0.7"), "ssh");
    EXPECT_STREQ(classifyType("xsess0", "1.2.3.4"), "ssh");  // host wins
}

TEST(SessionInfo, GuiSeatAndVtAreGraphical) {
    EXPECT_STREQ(classifyType("xsess0", ""), "graphical");
    EXPECT_STREQ(classifyType("ttyv0", ""), "graphical");
}

TEST(SessionInfo, LocalTtyIsTty) {
    EXPECT_STREQ(classifyType("ttyu0", ""), "tty");
    EXPECT_STREQ(classifyType("pts/1", ""), "tty");
}

// ── isGuiSeat ─────────────────────────────────────────────────────────────────

TEST(SessionInfo, GuiSeatDetection) {
    EXPECT_TRUE(isGuiSeat("xsess0"));
    EXPECT_TRUE(isGuiSeat("fbxseat1"));
    EXPECT_FALSE(isGuiSeat("ttyv0"));   // a VT is graphical but not OUR seat
    EXPECT_FALSE(isGuiSeat("pts/3"));
}

// ── sessionId ─────────────────────────────────────────────────────────────────

TEST(SessionInfo, SessionIdStableAndDistinct) {
    uint32_t a = sessionId("xsess0", "alice");
    EXPECT_EQ(a, sessionId("xsess0", "alice"));          // stable
    EXPECT_NE(a, sessionId("xsess0", "bob"));            // user matters
    EXPECT_NE(a, sessionId("ttyv0", "alice"));             // line matters
    EXPECT_NE(0u, a);                                       // never 0
}

TEST(SessionInfo, SessionIdFieldsNotConflated) {
    // "ab"+"c" must not collide with "a"+"bc".
    EXPECT_NE(sessionId("ab", "c"), sessionId("a", "bc"));
}

// ── buildPage ─────────────────────────────────────────────────────────────────

static fbx::shared::SessionEntry mk(const char* user) {
    fbx::shared::SessionEntry e{};
    std::strncpy(e.user_, user, sizeof(e.user_) - 1);
    return e;
}

TEST(SessionInfo, EmptyList) {
    auto pg = buildPage({}, 0);
    EXPECT_EQ(pg.total_, 0u);
    EXPECT_EQ(pg.count_, 0u);
    EXPECT_EQ(pg.has_more_, 0u);
}

TEST(SessionInfo, ExactlyThreeNoMore) {
    std::vector<fbx::shared::SessionEntry> v{mk("a"), mk("b"), mk("c")};
    auto pg = buildPage(v, 0);
    EXPECT_EQ(pg.count_, 3u);
    EXPECT_EQ(pg.total_, 3u);
    EXPECT_EQ(pg.has_more_, 0u);
}

TEST(SessionInfo, MoreThanThreePages) {
    std::vector<fbx::shared::SessionEntry> v{mk("a"), mk("b"), mk("c"), mk("d")};
    auto p0 = buildPage(v, 0);
    EXPECT_EQ(p0.count_, 3u);
    EXPECT_EQ(p0.has_more_, 1u);
    EXPECT_STREQ(p0.entries_[0].user_, "a");

    auto p1 = buildPage(v, 1);
    EXPECT_EQ(p1.count_, 1u);
    EXPECT_EQ(p1.has_more_, 0u);
    EXPECT_STREQ(p1.entries_[0].user_, "d");
    EXPECT_EQ(p1.total_, 4u);
}

TEST(SessionInfo, PageOutOfRange) {
    std::vector<fbx::shared::SessionEntry> v{mk("a")};
    auto pg = buildPage(v, 5);
    EXPECT_EQ(pg.count_, 0u);
    EXPECT_EQ(pg.has_more_, 0u);
    EXPECT_EQ(pg.total_, 1u);
}
