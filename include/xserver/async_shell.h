#pragma once
// AsyncShellCache — run shell commands OFF the compositor's single event-loop
// thread and cache their output.
//
// The compositor is single-threaded: if an XBus service handler shells out
// synchronously (pkg, wpa_cli, hccontrol, geom, ...) and that command hangs,
// the whole desktop freezes (no input, no cursor, no rendering). This helper
// lets a handler ask for a command's output without ever blocking: get()
// returns the last cached output immediately and kicks a background refresh.
// The first call for a command returns empty; subsequent calls return the most
// recent result (at worst one poll stale — fine for status widgets).

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

// Run `cmd` via `/bin/sh -c`, capturing stdout. Uses posix_spawn, NOT popen/system:
// the compositor is multithreaded, and popen()/system() fork()+run libc code
// (flockfile/fileno) in the child, which DEADLOCKS the child on a libc stdio mutex
// held by another thread at fork time (e.g. the main thread's std::cout, or a
// concurrent popen). The hung child never execs, lingers holding inherited fds
// (incl. the IPC listen socket), and wedges the whole compositor. posix_spawn execs
// the program directly without running unsafe libc code in the child, so it is safe
// to call from the background threads below. Returns the command's stdout; if
// exit_code is non-null it receives the command's exit status (WEXITSTATUS, or -1).
inline std::string spawnCaptureStatus(const std::string& cmd, int* exit_code) noexcept {
    if (exit_code) *exit_code = -1;
    int pf[2];
    if (::pipe2(pf, O_CLOEXEC) != 0) return {};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pf[1], STDOUT_FILENO);  // child stdout → pipe (dup2 clears CLOEXEC)
    const char* argv[] = {"/bin/sh", "-c", cmd.c_str(), nullptr};
    pid_t pid = -1;
    int rc = ::posix_spawn(&pid, "/bin/sh", &fa, nullptr,
                           const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(pf[1]);  // parent drops the write end so read() sees EOF at child exit
    if (rc != 0) { ::close(pf[0]); return {}; }
    std::string out;
    char buf[512]; ssize_t n;
    while ((n = ::read(pf[0], buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    ::close(pf[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (exit_code) *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return out;
}

// Capture stdout only (exit status ignored).
inline std::string spawnCapture(const std::string& cmd) noexcept {
    return spawnCaptureStatus(cmd, nullptr);
}

// Fork-safe replacement for system(): run `cmd` via /bin/sh -c, wait, return the
// exit code (WEXITSTATUS), or -1 on failure. Same posix_spawn rationale as above —
// system() forks and deadlocks the child on a libc lock in the threaded compositor.
inline int spawnStatus(const std::string& cmd) noexcept {
    const char* argv[] = {"/bin/sh", "-c", cmd.c_str(), nullptr};
    pid_t pid = -1;
    if (::posix_spawn(&pid, "/bin/sh", nullptr, nullptr,
                      const_cast<char* const*>(argv), environ) != 0) return -1;
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

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
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
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
    // Returns the cached stdout of `cmd`, refreshing it on a detached thread.
    // Never blocks. The refresh is hard-capped with timeout(1) so a hung command
    // can't leak a stuck thread forever.
    std::string get(const std::string& cmd, int timeout_sec = 4) {
        std::shared_ptr<Entry> e;
        {
            std::lock_guard<std::mutex> lk(map_mtx_);
            auto& slot = cache_[cmd];
            if (!slot) slot = std::make_shared<Entry>();
            e = slot;
        }
        bool expected = false;
        if (e->inflight.compare_exchange_strong(expected, true)) {
            std::string capped = "timeout -k 1 " + std::to_string(timeout_sec) + " " + cmd;
            std::thread([e, capped]() noexcept {
                std::string out = spawnCapture(capped);  // posix_spawn, fork-safe (see above)
                {
                    std::lock_guard<std::mutex> lk(e->out_mtx);
                    e->output = std::move(out);
                }
                e->inflight.store(false, std::memory_order_release);
            }).detach();
        }
        std::lock_guard<std::mutex> lk(e->out_mtx);
        return e->output;
    }

private:
    struct Entry {
        std::mutex        out_mtx;
        std::string       output;
        std::atomic<bool> inflight{false};
    };
    std::map<std::string, std::shared_ptr<Entry>> cache_;
    std::mutex map_mtx_;
};

// Process-wide cache shared by all services (keyed by command string).
inline AsyncShellCache g_asyncShell;

// Fire-and-forget: run a side-effecting command (set brightness, start a
// service, install a package, ...) on a detached thread so it never blocks the
// compositor event loop. The result is not observed — use this only where the
// handler doesn't need the exit status.
inline void runDetached(std::string cmd) noexcept {
    // posix_spawn (not system()) for the same fork-safety reason as spawnCapture;
    // reap on the detached thread so the child doesn't become a zombie.
    std::thread([cmd = std::move(cmd)]() noexcept {
        const char* argv[] = {"/bin/sh", "-c", cmd.c_str(), nullptr};
        pid_t pid = -1;
        if (::posix_spawn(&pid, "/bin/sh", nullptr, nullptr,
                          const_cast<char* const*>(argv), environ) == 0) {
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    }).detach();
}

// Bounded-blocking: run a command and return its real stdout, but hard-cap it
// with timeout(1) so it can't hang the event loop forever. Use for
// USER-TRIGGERED one-shot queries (disk list, font list, ...) where an empty
// "cached-first" result would be worse than a brief, bounded wait.
inline std::string runCapped(const std::string& cmd, int timeout_sec = 5) noexcept {
    // posix_spawn-based (see spawnCapture) — fork-safe in the multithreaded compositor.
    return spawnCapture("timeout -k 1 " + std::to_string(timeout_sec) + " " + cmd);
}

} // namespace fbx::xserver
