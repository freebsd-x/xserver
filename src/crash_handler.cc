#include "xserver/crash_handler.h"

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits.h>

namespace fbx::xserver {
namespace {

// Precomputed at install time so the handler does no path formatting.
char g_crash_path[PATH_MAX];
bool g_have_path = false;

// Dedicated stack so the handler can run even after a stack overflow.
char g_altstack[65536];

const char* sigName(int s) noexcept {
    switch (s) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        default:      return "fatal signal";
    }
}

// ── async-signal-safe output helpers (no malloc, no stdio) ────────────────────
void w(int fd, const char* s) noexcept {
    if (s) { ssize_t r = write(fd, s, std::strlen(s)); (void)r; }
}
void wHex(int fd, unsigned long v) noexcept {
    static const char hex[] = "0123456789abcdef";
    char buf[2 + sizeof(v) * 2];
    buf[0] = '0'; buf[1] = 'x';
    for (unsigned i = 0; i < sizeof(v) * 2; ++i)
        buf[2 + i] = hex[(v >> ((sizeof(v) * 2 - 1 - i) * 4)) & 0xf];
    ssize_t r = write(fd, buf, sizeof(buf)); (void)r;
}

void dumpTo(int fd, int sig, void* fault_addr) noexcept {
    w(fd, "\n=== FreeBSD-X crash: ");
    w(fd, sigName(sig));
    if (sig == SIGSEGV || sig == SIGBUS) {
        w(fd, " @ ");
        wHex(fd, reinterpret_cast<unsigned long>(fault_addr));
    }
    w(fd, " ===\n");
    void* frames[64];
    int n = backtrace(frames, 64);
    // backtrace_symbols_fd writes directly to the fd and does not allocate,
    // so it is safe to call from a signal handler.
    backtrace_symbols_fd(frames, n, fd);
}

extern "C" void crashHandler(int sig, siginfo_t* info, void*) noexcept {
    void* fault_addr = info ? info->si_addr : nullptr;

    // Always to stderr (captured in service/compositor logs).
    dumpTo(STDERR_FILENO, sig, fault_addr);

    // Best-effort persistent record for offline triage.
    if (g_have_path) {
        int fd = open(g_crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { dumpTo(fd, sig, fault_addr); (void)fsync(fd); close(fd); }
    }

    // Restore the default disposition and re-raise so a core is still produced.
    signal(sig, SIG_DFL);
    raise(sig);
}

}  // namespace

void installCrashHandler(const char* proc_name) noexcept {
    // Build the spool path once, here (not in the handler).
    const char* home = getenv("HOME");
    if (home && *home) {
        char lvl1[PATH_MAX];
        char dir[PATH_MAX];
        if (std::snprintf(lvl1, sizeof(lvl1), "%s/.freebsdx", home) > 0 &&
            std::snprintf(dir,  sizeof(dir),  "%s/.freebsdx/crashes", home) > 0) {
            mkdir(lvl1, 0700);   // ignore EEXIST
            mkdir(dir,  0700);
            if (std::snprintf(g_crash_path, sizeof(g_crash_path), "%s/%s-%d.crash",
                              dir, proc_name ? proc_name : "freebsdx",
                              static_cast<int>(getpid())) > 0)
                g_have_path = true;
        }
    }

    stack_t ss{};
    ss.ss_sp = g_altstack;
    ss.ss_size = sizeof(g_altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = crashHandler;
    sigemptyset(&sa.sa_mask);
    // SA_RESETHAND: if the handler itself faults, the second delivery takes the
    // default action instead of looping.
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    for (int s : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL})
        sigaction(s, &sa, nullptr);
}

}  // namespace fbx::xserver
