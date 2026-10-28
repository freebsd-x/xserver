// Pure parsers for the per-user services (CronJobs/SshKeys/Captures/EnvVars).

#include "xserver/user_parse.h"
#include "shared/user_env.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::userdata;
namespace userenv = fbx::shared::userenv;

// ── crontab ──────────────────────────────────────────────────────────────────

constexpr std::string_view kCrontab =
    "SHELL=/bin/sh\n"                       // 1  env assignment, not a job
    "# nightly backup\n"                    // 2  prose comment, not a job
    "0 3 * * * /home/u/bin/backup\n"        // 3
    "#*/5 * * * * fetchmail -s\n"           // 4  disabled
    "@reboot   /usr/local/bin/syncthing\n"  // 5
    "\n"                                    // 6
    "# 30 2 * * mon-fri  date +\\%F >> /tmp/log %stdin\n";  // 7 disabled

TEST(UserCron, ParsesEnabledDisabledAndKeywords) {
    auto jobs = parseCrontab(kCrontab);
    ASSERT_EQ(jobs.size(), 4u);
    EXPECT_EQ(jobs[0].line, 3u);
    EXPECT_EQ(jobs[0].schedule, "0 3 * * *");
    EXPECT_EQ(jobs[0].command, "/home/u/bin/backup");
    EXPECT_TRUE(jobs[0].enabled);
    EXPECT_EQ(jobs[1].line, 4u);
    EXPECT_EQ(jobs[1].schedule, "*/5 * * * *");
    EXPECT_FALSE(jobs[1].enabled);
    EXPECT_EQ(jobs[2].schedule, "@reboot");
    EXPECT_EQ(jobs[2].command, "/usr/local/bin/syncthing");
    EXPECT_EQ(jobs[3].line, 7u);
    EXPECT_EQ(jobs[3].schedule, "30 2 * * mon-fri");
    EXPECT_FALSE(jobs[3].enabled);
    EXPECT_TRUE(parseCrontab("").empty());
}

TEST(UserCron, RejectsMalformedLines) {
    CronJob j;
    EXPECT_FALSE(parseCronLine("0 3 * *", j));                  // too few fields
    EXPECT_FALSE(parseCronLine("0 3 * * *", j));                // no command
    EXPECT_FALSE(parseCronLine("@sometimes echo hi", j));       // unknown keyword
    EXPECT_FALSE(parseCronLine("MAILTO=me@example.com", j));
}

TEST(UserCron, ToggleRewritesOnlyThatLine) {
    std::string out;
    ASSERT_TRUE(setCronEnabled(kCrontab, 3, false, out));
    auto jobs = parseCrontab(out);
    ASSERT_EQ(jobs.size(), 4u);
    EXPECT_FALSE(jobs[0].enabled);
    EXPECT_EQ(jobs[0].line, 3u);              // ids stay stable across toggles

    ASSERT_TRUE(setCronEnabled(out, 3, true, out));
    ASSERT_TRUE(setCronEnabled(out, 4, true, out));
    jobs = parseCrontab(out);
    EXPECT_TRUE(jobs[0].enabled);
    EXPECT_TRUE(jobs[1].enabled);
    EXPECT_NE(out.find("\n*/5 * * * * fetchmail -s\n"), std::string::npos);
    EXPECT_NE(out.find("SHELL=/bin/sh\n# nightly backup\n"), std::string::npos);

    // Toggling to the state it's already in is a no-op success.
    std::string same;
    ASSERT_TRUE(setCronEnabled(out, 3, true, same));
    EXPECT_EQ(same, out);
}

TEST(UserCron, ToggleRejectsStaleIds) {
    std::string out;
    EXPECT_FALSE(setCronEnabled(kCrontab, 0, true, out));
    EXPECT_FALSE(setCronEnabled(kCrontab, 1, true, out));    // SHELL=
    EXPECT_FALSE(setCronEnabled(kCrontab, 2, true, out));    // prose comment
    EXPECT_FALSE(setCronEnabled(kCrontab, 99, true, out));
}

TEST(UserCron, RunnableCommandStopsAtPercent) {
    EXPECT_EQ(cronRunnableCommand("date +\\%F >> /tmp/log %stdin"), "date +%F >> /tmp/log ");
    EXPECT_EQ(cronRunnableCommand("echo hi"), "echo hi");
}

// ── authorized_keys ──────────────────────────────────────────────────────────

constexpr std::string_view kAuthKeys =
    "# laptop\n"
    "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIExample wheel@laptop\n"
    "from=\"10.0.0.0/8\",command=\"echo ssh-rsa hi\" ssh-rsa AAAAB3Nza backup key\n"
    "ecdsa-sha2-nistp256 AAAAE2VjZHNh\n"
    "sk-ssh-ed25519@openssh.com AAAAGnNr yubikey\n"
    "garbage line\n";

TEST(UserSsh, ParsesKeysOptionsAndComments) {
    auto keys = parseAuthorizedKeys(kAuthKeys);
    ASSERT_EQ(keys.size(), 4u);
    EXPECT_EQ(keys[0].line, 2u);
    EXPECT_EQ(keys[0].algo, "ssh-ed25519");
    EXPECT_EQ(keys[0].comment, "wheel@laptop");
    // The quoted "ssh-rsa" inside command= must not be taken as the key type.
    EXPECT_EQ(keys[1].algo, "ssh-rsa");
    EXPECT_EQ(keys[1].comment, "backup key");
    EXPECT_EQ(keys[2].algo, "ecdsa-p256");
    EXPECT_EQ(keys[2].comment, "");
    EXPECT_EQ(keys[3].algo, "sk-ed25519");
    for (const auto& k : keys) EXPECT_LE(k.algo.size(), 11u);
}

TEST(UserSsh, RemoveDropsOnlyThatKey) {
    std::string out;
    ASSERT_TRUE(removeAuthorizedKey(kAuthKeys, 3, out));
    auto keys = parseAuthorizedKeys(out);
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(out.find("backup key"), std::string::npos);
    EXPECT_NE(out.find("# laptop\n"), std::string::npos);
    EXPECT_FALSE(removeAuthorizedKey(kAuthKeys, 1, out));    // a comment
    EXPECT_FALSE(removeAuthorizedKey(kAuthKeys, 6, out));    // garbage
    EXPECT_FALSE(removeAuthorizedKey(kAuthKeys, 42, out));
}

TEST(UserSsh, CountsKnownHosts) {
    EXPECT_EQ(countKnownHosts("github.com ssh-ed25519 AAA\n# c\n\n|1|hash= ecdsa AAA\n"), 2u);
    EXPECT_EQ(countKnownHosts(""), 0u);
}

// ── captures ─────────────────────────────────────────────────────────────────

TEST(UserCaptures, SafeNames) {
    EXPECT_TRUE(safeCaptureName("screenshot-20261027-101500.png"));
    EXPECT_FALSE(safeCaptureName(""));
    EXPECT_FALSE(safeCaptureName("../.ssh/authorized_keys"));
    EXPECT_FALSE(safeCaptureName("sub/file.png"));
    EXPECT_FALSE(safeCaptureName(".."));
    EXPECT_FALSE(safeCaptureName(".hidden"));
}

// ── user env file ────────────────────────────────────────────────────────────

TEST(UserEnv, ParseSkipsJunkAndKeepsLastValue) {
    auto v = userenv::parse("# header\nEDITOR=vim\n\nbad line\n1X=no\nPATH=/a:/b=c\nEDITOR=nvi\r\n");
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0].key, "EDITOR");
    EXPECT_EQ(v[0].value, "nvi");
    EXPECT_EQ(v[1].key, "PATH");
    EXPECT_EQ(v[1].value, "/a:/b=c");     // value runs to end of line
}

TEST(UserEnv, SetUnsetRoundTrip) {
    std::vector<userenv::Var> v;
    EXPECT_TRUE(userenv::set(v, "GDK_SCALE", "2"));
    EXPECT_TRUE(userenv::set(v, "EMPTY", ""));
    EXPECT_TRUE(userenv::set(v, "GDK_SCALE", "1"));
    EXPECT_FALSE(userenv::set(v, "BAD-KEY", "x"));
    EXPECT_FALSE(userenv::set(v, "INJECT", "a\nLD_PRELOAD=/x.so"));
    auto back = userenv::parse(userenv::serialize(v));
    ASSERT_EQ(back.size(), 2u);
    EXPECT_EQ(back[0].value, "1");
    EXPECT_EQ(back[1].key, "EMPTY");
    EXPECT_TRUE(userenv::unset(back, "EMPTY"));
    EXPECT_FALSE(userenv::unset(back, "EMPTY"));
    EXPECT_EQ(back.size(), 1u);
}
