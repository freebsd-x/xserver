#pragma once

#include <sys/types.h>
#include <memory>

namespace fbx::xserver {

// A live login session held open against xsessiond. Authenticates the user,
// asks the daemon to open the PAM session + utmpx login record, and keeps the
// connection open for the session's lifetime — closing it (destructor or
// end()) tells the daemon to log the user out (utmpx DEAD_PROCESS,
// pam_close_session). Best-effort: open() returns nullptr if the daemon is
// unreachable or the session can't be established, and callers should carry on
// without it (login accounting simply won't be recorded). See
// docs/xsessiond-design.md and xserver/xsessiond_protocol.h.
class XsessiondSession {
public:
    // Connect, Authenticate(user, password), StartSession(leader pid). Returns a
    // live session on success, or nullptr if the daemon is down / auth is
    // rejected / the session can't open.
    static std::unique_ptr<XsessiondSession> open(const char* user,
                                              const char* password,
                                              pid_t leader) noexcept;

    ~XsessiondSession();  // closes the connection → daemon ends the session
    void end() noexcept;  // explicit EndSession + close, idempotent

    XsessiondSession(const XsessiondSession&) = delete;
    XsessiondSession& operator=(const XsessiondSession&) = delete;

private:
    explicit XsessiondSession(int fd) noexcept : fd_(fd) {}
    int fd_;
};

} // namespace fbx::xserver
