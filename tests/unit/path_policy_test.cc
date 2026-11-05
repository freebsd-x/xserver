#include "xserver/path_policy.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace fbx::xserver::pathpolicy;

namespace {
class PathPolicy : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/fbx-pathpolicy.XXXXXX";
        ASSERT_NE(::mkdtemp(tmpl), nullptr);
        base_ = fs::canonical(tmpl);
        home_ = base_ / "me";
        fs::create_directories(home_ / "docs");
        fs::create_directories(base_ / "mean");
        fs::create_directories(base_ / "secret");
        std::ofstream(home_ / "docs" / "a.txt") << "a";
    }
    void TearDown() override { fs::remove_all(base_); }
    std::string within(const fs::path& p) { return resolveWithin(p.string(), {home_.c_str()}); }
    fs::path base_, home_;
};
}  // namespace

TEST(PathPolicyInside, NeedsAPathBoundary) {
    EXPECT_TRUE(inside("/home/me", "/home/me"));
    EXPECT_TRUE(inside("/home/me/x", "/home/me"));
    EXPECT_FALSE(inside("/home/mean", "/home/me"));
    EXPECT_FALSE(inside("/home", "/home/me"));
    EXPECT_FALSE(inside("/home/me", ""));
}

TEST_F(PathPolicy, ResolvesWhatsInside) {
    EXPECT_EQ(within(home_), home_.string());
    EXPECT_EQ(within(home_ / "docs" / ".." / "docs" / "a.txt"), (home_ / "docs" / "a.txt").string());
}

TEST_F(PathPolicy, RefusesWayOut) {
    EXPECT_EQ(within(home_ / ".." / "secret"), "");
    EXPECT_EQ(within(base_ / "mean"), "");       // shares the prefix only
    EXPECT_EQ(within(home_ / "missing"), "");    // nothing there to vouch for
    EXPECT_EQ(within(""), "");
}

TEST_F(PathPolicy, RefusesSymlinkOutButFollowsOneIn) {
    fs::create_directory_symlink(base_ / "secret", home_ / "out");
    fs::create_directory_symlink(home_ / "docs", base_ / "in");
    EXPECT_EQ(within(home_ / "out"), "");
    EXPECT_EQ(within(base_ / "in"), (home_ / "docs").string());
}

TEST_F(PathPolicy, MissingRootsDontMatch) {
    EXPECT_EQ(resolveWithin(home_.string(), {nullptr, "", "/nonexistent-root"}), "");
    EXPECT_EQ(resolveWithin(home_.string(), {nullptr, home_.c_str()}), home_.string());
}
