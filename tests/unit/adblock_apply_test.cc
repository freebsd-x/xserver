// The root side of AdBlock (xsessiond adblock.apply), run unprivileged against
// a temporary home and hosts file: what it writes, and what it refuses to read.

#include "xserver/adblock_apply.h"
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace fbx::xserver::adblock;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

class AdBlockApply : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/adblock_apply_test.XXXXXX";
        root_ = ::mkdtemp(tmpl);
        home_ = root_ / "home";
        hosts_ = root_ / "hosts";
        fs::create_directories(home_ / ".cache/freebsd-x/adblock");
        std::ofstream(hosts_) << "127.0.0.1 localhost\n192.168.1.5 nas.lan\n";
    }
    void TearDown() override { fs::remove_all(root_); }
    void writeList(const std::string& body) {
        std::ofstream(home_ / ".cache/freebsd-x/adblock/domains") << body;
    }
    int32_t apply(bool on, uid_t uid = ::getuid()) {
        return applyToHosts(on, uid, home_.string(), hosts_.string());
    }
    fs::path root_, home_, hosts_;
};

}  // namespace

TEST_F(AdBlockApply, WritesOnlySinkholeLinesAndRemovesThemAgain) {
    writeList("ads.example.com\n"
              "1.2.3.4 bank.com\n"            // a redirect smuggled into the list
              "0.0.0.0 tracker.net\n"
              "pkg.freebsd.org\n");          // never blocked
    ASSERT_EQ(apply(true), 0);
    std::string on = slurp(hosts_);
    EXPECT_EQ(on.find("127.0.0.1 localhost\n192.168.1.5 nas.lan\n"), 0u);
    EXPECT_EQ(blockedCount(on), 2u);
    EXPECT_NE(on.find("0.0.0.0 ads.example.com\n"), std::string::npos);
    EXPECT_EQ(on.find("bank.com"), std::string::npos);
    EXPECT_EQ(on.find("1.2.3.4"), std::string::npos);
    EXPECT_EQ(on.find("pkg.freebsd.org"), std::string::npos);
    EXPECT_FALSE(fs::exists(hosts_.string() + ".fbx-adblock"));   // temp file renamed away

    ASSERT_EQ(apply(false), 0);
    EXPECT_EQ(slurp(hosts_), "127.0.0.1 localhost\n192.168.1.5 nas.lan\n");
}

TEST_F(AdBlockApply, MissingOrEmptyListIsRefused) {
    EXPECT_EQ(apply(true), -ENOENT);
    writeList("# nothing usable\n1.2.3.4 bank.com\n");
    EXPECT_EQ(apply(true), -ENODATA);
    EXPECT_EQ(slurp(hosts_), "127.0.0.1 localhost\n192.168.1.5 nas.lan\n");
}

TEST_F(AdBlockApply, RefusesSymlinksInThePath) {
    // A directory component replaced by a symlink to somewhere else.
    fs::path elsewhere = root_ / "elsewhere";
    fs::create_directories(elsewhere / "freebsd-x/adblock");
    std::ofstream(elsewhere / "freebsd-x/adblock/domains") << "ads.example.com\n";
    fs::remove_all(home_ / ".cache");
    fs::create_directory_symlink(elsewhere, home_ / ".cache");
    EXPECT_EQ(apply(true), -ENOENT);
}

TEST_F(AdBlockApply, RefusesASymlinkedListFile) {
    fs::path secret = root_ / "secret";
    std::ofstream(secret) << "ads.example.com\n";
    fs::create_symlink(secret, home_ / ".cache/freebsd-x/adblock/domains");
    EXPECT_EQ(apply(true), -ENOENT);
}

TEST_F(AdBlockApply, RefusesAListOwnedBySomeoneElse) {
    writeList("ads.example.com\n");
    EXPECT_EQ(apply(true, ::getuid() + 1), -ENOENT);   // not that user's files
    EXPECT_EQ(blockedCount(slurp(hosts_)), 0u);
}
