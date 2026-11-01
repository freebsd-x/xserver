// The compositor's posix_spawn helper: environment overrides, output
// redirection and a new process group reach the child.

#include "xserver/spawn_util.h"
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/wait.h>

using namespace fbx::xserver;

namespace {
std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
int waitExit(pid_t pid) {
    int st = 0;
    if (::waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
}  // namespace

TEST(SpawnUtil, FirstExecutablePicksTheFirstThatExists) {
    EXPECT_EQ(spawn::firstExecutable({"/nonexistent/x", "/bin/sh", "/usr/bin/env"}), "/bin/sh");
    EXPECT_EQ(spawn::firstExecutable({"/nonexistent/x"}), "");
}

TEST(SpawnUtil, EnvOverridesAndOutputRedirect) {
    ::setenv("SPAWN_UTIL_KEEP", "kept", 1);
    ::setenv("SPAWN_UTIL_OVERRIDE", "old", 1);
    char tmpl[] = "/tmp/spawn_util_test.XXXXXX";
    int fd = ::mkstemp(tmpl);
    ASSERT_GE(fd, 0);
    ::close(fd);
    pid_t pid = spawn::start("/bin/sh", "sh",
                             {"-c", "echo \"$SPAWN_UTIL_KEEP $SPAWN_UTIL_OVERRIDE $SPAWN_UTIL_NEW\"; echo err >&2"},
                             {.env = {{"SPAWN_UTIL_OVERRIDE", "new"}, {"SPAWN_UTIL_NEW", "added"}},
                              .stdout_path = tmpl});
    ASSERT_GT(pid, 0);
    EXPECT_EQ(waitExit(pid), 0);
    EXPECT_EQ(slurp(tmpl), "kept new added\nerr\n");   // stderr follows stdout
    ::unlink(tmpl);
}

TEST(SpawnUtil, NewProcessGroup) {
    pid_t pid = spawn::start("/bin/sh", "sh", {"-c", "sleep 0.3"}, {.new_group = true});
    ASSERT_GT(pid, 0);
    EXPECT_EQ(::getpgid(pid), pid);   // leads its own group
    EXPECT_EQ(waitExit(pid), 0);
}

TEST(SpawnUtil, MissingProgramFails) {
    EXPECT_EQ(spawn::start("", "x", {}), -1);
    EXPECT_EQ(spawn::start("/nonexistent/program", "x", {}), -1);
}
