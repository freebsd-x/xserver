// Unit tests for pure Process (sort/page) + SysLog (line parse) helpers.

#include "xserver/proc_list.h"
#include "xserver/syslog_parse.h"
#include <gtest/gtest.h>
#include <cstring>

// ── Process ───────────────────────────────────────────────────────────────────

static fbx::shared::ProcessEntry proc(const char* name, uint32_t cpu10) {
    fbx::shared::ProcessEntry e{};
    std::strncpy(e.name_, name, sizeof(e.name_) - 1);
    e.cpu_pct10_ = cpu10;
    return e;
}

TEST(ProcList, SortsByCpuDescendingStable) {
    std::vector<fbx::shared::ProcessEntry> v{
        proc("a", 100), proc("b", 500), proc("c", 500), proc("d", 50)};
    fbx::xserver::proc::sortByCpuDesc(v);
    EXPECT_STREQ(v[0].name_, "b");   // 500, first of the ties (stable)
    EXPECT_STREQ(v[1].name_, "c");   // 500
    EXPECT_STREQ(v[2].name_, "a");   // 100
    EXPECT_STREQ(v[3].name_, "d");   // 50
}

TEST(ProcList, BuildPagePaging) {
    std::vector<fbx::shared::ProcessEntry> v{
        proc("a", 1), proc("b", 1), proc("c", 1), proc("d", 1)};
    auto p0 = fbx::xserver::proc::buildPage(v, 0);
    EXPECT_EQ(p0.count_, 3u);
    EXPECT_EQ(p0.total_, 4u);
    EXPECT_EQ(p0.has_more_, 1u);
    auto p1 = fbx::xserver::proc::buildPage(v, 1);
    EXPECT_EQ(p1.count_, 1u);
    EXPECT_EQ(p1.has_more_, 0u);
    EXPECT_STREQ(p1.entries_[0].name_, "d");
}

// ── SysLog line parsing ───────────────────────────────────────────────────────

TEST(SyslogParse, TagWithPid) {
    auto e = fbx::xserver::syslog::parseLine(
        "Sep 20 14:30:01 host sshd[123]: Accepted password for bob");
    EXPECT_EQ(e.facility, "sshd");
    EXPECT_EQ(e.message, "Accepted password for bob");
}

TEST(SyslogParse, TagWithoutPid) {
    auto e = fbx::xserver::syslog::parseLine(
        "Sep 20 14:30:02 host kernel: link state changed to UP");
    EXPECT_EQ(e.facility, "kernel");
    EXPECT_EQ(e.message, "link state changed to UP");
}

TEST(SyslogParse, TrailingNewlineStripped) {
    auto e = fbx::xserver::syslog::parseLine("Sep 20 14:30:03 host ntpd: adjusting\r\n");
    EXPECT_EQ(e.facility, "ntpd");
    EXPECT_EQ(e.message, "adjusting");
}

TEST(SyslogParse, MalformedLineBecomesMessage) {
    auto e = fbx::xserver::syslog::parseLine("garbage");
    EXPECT_EQ(e.facility, "");
    EXPECT_EQ(e.message, "garbage");
}
