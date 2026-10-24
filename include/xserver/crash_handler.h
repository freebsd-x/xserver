#pragma once

namespace fbx::xserver {

// Install fatal-signal handlers (SIGSEGV/SIGABRT/SIGBUS/SIGFPE/SIGILL) that dump
// a backtrace to stderr and to ~/.freebsdx/crashes/<proc_name>-<pid>.crash, then
// restore the default action and re-raise so the kernel still writes a core.
//
// Call once, early in main(). The handler runs on an alternate signal stack so
// it can unwind even on stack overflow, and does only async-signal-safe work.
// proc_name labels the spool file (e.g. "freebsdx", "xaudio").
void installCrashHandler(const char* proc_name) noexcept;

}  // namespace fbx::xserver
