#pragma once
// AsyncShellCache — run status commands OFF the compositor's single event-loop
// thread and cache their output.
//
// The compositor is single-threaded: if an XBus service handler runs a
// command synchronously (pkg, wpa_cli, hccontrol, geom, ...) and that command
// hangs, the whole desktop freezes (no input, no cursor, no rendering). This
// helper lets a handler ask for a command's output without ever blocking:
// get() returns the last cached output immediately and kicks a background
// refresh. The first call returns empty; subsequent calls return the most
// recent result (at worst one poll stale — fine for status widgets). Never use
// it for a command with a side effect, or one whose answer must be current.

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstdio>
#include <spawn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

extern "C" char** environ;  // global; POSIX doesn't guarantee a header declares it

namespace fbx::xserver {

// Process spawning for the compositor's services. All of it is argv-based:
// there is no /bin/sh here, so no argument (an SSID, a device, a path, a
// printer, a package) can be read as shell syntax — enforced by
// scripts/lint-no-shell.py. Everything uses posix_spawn, NOT popen/system: the
// compositor is multithreaded, and popen()/system() fork() and run libc code
// in the child, which deadlocks it on a libc lock another thread held at fork
// time; posix_spawn execs directly.

// posix_spawn a fixed argv (no shell — wire-supplied arguments can't be
// interpreted) and wait; returns the exit status, or -1. argv[0] is a path.
inline int spawnArgvStatus(const std::vector<std::string>& args) noexcept {
    if (args.empty()) return -1;
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    if (::posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return -1;
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Run a program with an argv — no shell, so no argument (an SSID, a device, a
// path) can ever be interpreted as shell syntax — and capture its stdout.
// args[0] is looked up on PATH like the shell did. Options: `stdin_data` is
// written to its stdin (secrets such as an API key go here, never in argv,
// which any local user can read with ps); `timeout_sec` > 0 caps it with
// timeout(1); stderr goes to /dev/null. `exit_code` gets WEXITSTATUS or -1.
struct ArgvRun {
    std::string stdin_data;
    int         timeout_sec = 0;
    int*        exit_code = nullptr;
    bool        merge_stderr = false;   // stderr into the captured output (like 2>&1), for error messages
};
inline std::string spawnArgvCapture(std::vector<std::string> args, const ArgvRun& opt = {}) noexcept {
    if (opt.exit_code) *opt.exit_code = -1;
    if (args.empty()) return {};
    if (opt.timeout_sec > 0)
        args.insert(args.begin(), {"timeout", "-k", "2", std::to_string(opt.timeout_sec)});
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    int out_pf[2], in_pf[2] = {-1, -1};
    if (::pipe2(out_pf, O_CLOEXEC) != 0) return {};
    if (::pipe2(in_pf, O_CLOEXEC) != 0) { ::close(out_pf[0]); ::close(out_pf[1]); return {}; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in_pf[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, out_pf[1], STDOUT_FILENO);
    if (opt.merge_stderr) posix_spawn_file_actions_adddup2(&fa, out_pf[1], STDERR_FILENO);
    else                  posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    int rc = ::posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(in_pf[0]);
    ::close(out_pf[1]);
    if (rc != 0) { ::close(in_pf[1]); ::close(out_pf[0]); return {}; }
    // Small inputs (headers, a config line) fit the pipe buffer, so writing
    // them all before reading can't deadlock against the child's stdout.
    size_t off = 0;
    while (off < opt.stdin_data.size()) {
        ssize_t w = ::write(in_pf[1], opt.stdin_data.data() + off, opt.stdin_data.size() - off);
        if (w <= 0) break;
        off += static_cast<size_t>(w);
    }
    ::close(in_pf[1]);
    std::string out;
    char buf[512]; ssize_t n;
    while ((n = ::read(out_pf[0], buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    ::close(out_pf[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (opt.exit_code) *opt.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return out;
}

// The exit status of an argv run (output discarded). See spawnArgvCapture.
inline int spawnArgvRun(std::vector<std::string> args, int timeout_sec = 0) noexcept {
    int code = -1;
    (void)spawnArgvCapture(std::move(args), {.timeout_sec = timeout_sec, .exit_code = &code});
    return code;
}

class AsyncShellCache {
public:
    // Returns the cached stdout of the program `args` (argv, no shell),
    // refreshing it on a detached thread. Never blocks. The refresh is
    // hard-capped with timeout(1) so a hung command can't leak a stuck thread.
    std::string get(std::vector<std::string> args, int timeout_sec = 4) {
        auto e = refresh(std::move(args), timeout_sec);
        std::lock_guard<std::mutex> lk(e->out_mtx);
        return e->output;
    }

    // Did the program's last (cached) run exit 0? Same never-blocking refresh
    // as get(); false until a first run has finished.
    bool succeeded(std::vector<std::string> args, int timeout_sec = 4) {
        auto e = refresh(std::move(args), timeout_sec);
        std::lock_guard<std::mutex> lk(e->out_mtx);
        return e->exit_code == 0;
    }

private:
    struct Entry {
        std::mutex        out_mtx;
        std::string       output;
        int               exit_code = -1;
        std::atomic<bool> inflight{false};
    };

    std::shared_ptr<Entry> refresh(std::vector<std::string> args, int timeout_sec) {
        std::string key;
        for (const auto& a : args) { key += a; key += '\x1f'; }
        std::shared_ptr<Entry> e;
        {
            std::lock_guard<std::mutex> lk(map_mtx_);
            auto& slot = cache_[key];
            if (!slot) slot = std::make_shared<Entry>();
            e = slot;
        }
        bool expected = false;
        if (e->inflight.compare_exchange_strong(expected, true)) {
            std::thread([e, args = std::move(args), timeout_sec]() mutable noexcept {
                int code = -1;
                std::string out = spawnArgvCapture(std::move(args), {.timeout_sec = timeout_sec, .exit_code = &code});
                {
                    std::lock_guard<std::mutex> lk(e->out_mtx);
                    e->output = std::move(out);
                    e->exit_code = code;
                }
                e->inflight.store(false, std::memory_order_release);
            }).detach();
        }
        return e;
    }

    std::map<std::string, std::shared_ptr<Entry>> cache_;
    std::mutex map_mtx_;
};

// Process-wide cache shared by all services (keyed by argv).
inline AsyncShellCache g_asyncShell;

// Fire-and-forget: run a side-effecting program (start a service, ...) on a
// detached thread so it never blocks the compositor event loop. The result is
// not observed — use this only where the handler doesn't need the exit status.
inline void runDetached(std::vector<std::string> args) noexcept {
    std::thread([args = std::move(args)]() mutable noexcept {
        (void)spawnArgvRun(std::move(args));   // waits, so the child is reaped
    }).detach();
}

// Bounded-blocking: run a program and return its real stdout, hard-capped with
// timeout(1) so it can't hang the caller. For USER-TRIGGERED one-shot queries
// (disk list, font list, ...) where an empty "cached-first" result would be
// worse than a brief, bounded wait.
inline std::string runCapped(std::vector<std::string> args, int timeout_sec = 5) noexcept {
    return spawnArgvCapture(std::move(args), {.timeout_sec = timeout_sec});
}

} // namespace fbx::xserver
