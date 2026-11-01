#pragma once

#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" char** environ;

// Start a helper process from the multi-threaded compositor without fork().
// After fork() only async-signal-safe calls are allowed in the child; the old
// spawns called setenv(), exit(), std::cerr or built strings there, any of
// which can deadlock on a lock another thread held at fork time — leaving the
// lock screen, greeter, screensaver or shell never started. Everything here is
// prepared in the parent, and posix_spawn execs directly.
namespace fbx::xserver::spawn {

struct Options {
    std::vector<std::pair<std::string, std::string>> env;   // set / override in the child
    std::string stdout_path;   // non-empty: stdout+stderr go here (truncated)
    bool new_group = false;    // lead a new process group (signal it as a unit)
};

// The first of `candidates` that is executable, or "".
inline std::string firstExecutable(const std::vector<std::string>& candidates) {
    for (const auto& c : candidates)
        if (::access(c.c_str(), X_OK) == 0) return c;
    return {};
}

// Run `path` with argv = {argv0, args...}. Returns the child's pid, or -1.
inline pid_t start(const std::string& path, const std::string& argv0,
                   const std::vector<std::string>& args, const Options& opt = {}) noexcept {
    if (path.empty()) return -1;
    std::vector<std::string> env_strs;
    for (char** e = environ; e && *e; ++e) {
        std::string kv = *e;
        bool overridden = false;
        for (const auto& [k, v] : opt.env)
            if (kv.compare(0, k.size() + 1, k + "=") == 0) { overridden = true; break; }
        if (!overridden) env_strs.push_back(std::move(kv));
    }
    for (const auto& [k, v] : opt.env) env_strs.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& s : env_strs) envp.push_back(s.data());
    envp.push_back(nullptr);

    std::vector<std::string> argv_strs{argv0};
    argv_strs.insert(argv_strs.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& s : argv_strs) argv.push_back(s.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&attr);
    if (!opt.stdout_path.empty()) {
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, opt.stdout_path.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);
    }
    if (opt.new_group) {
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);
    }
    pid_t pid = -1;
    if (::posix_spawn(&pid, path.c_str(), &fa, &attr, argv.data(), envp.data()) != 0) pid = -1;
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    return pid;
}

}  // namespace fbx::xserver::spawn
