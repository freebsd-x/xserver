#pragma once

namespace fbx::xserver {

// Try the setuid-root xauthcheck helper to verify a password. Returns 1 on
// auth success, 0 on auth failure, and -1 if the helper is unavailable (not
// installed setuid root) — in which case the caller should fall back to
// in-process PAM, which only works when the compositor itself runs as root.
//
// Keeping the privileged PAM check in a tiny setuid helper lets the compositor
// (freebsdx) stay unprivileged; both the login greeter and the screen locker
// route through this. Definition lives in login_service.cc.
int authViaHelper(const char* user, const char* password) noexcept;

// Try the xsessiond daemon (phase 1 of the unified seat/session daemon — see
// docs/xsessiond-design.md). Same 1/0/-1 contract as authViaHelper: 1 success,
// 0 reject, -1 if the daemon isn't reachable (caller falls back to the setuid
// helper, then in-process PAM). Definition in xsessiond_client.cc.
int authViaXsessiond(const char* user, const char* password) noexcept;

// Full password check for `service`: tries xsessiond → setuid helper →
// in-process PAM (the last only works if the compositor runs as root). The
// in-process path runs pam_acct_mgmt after pam_authenticate, so disabled/
// expired/locked accounts are rejected even with a correct password. Shared by
// the login greeter and the screen locker so both enforce the same policy.
// Definition in login_service.cc. Does NOT throttle — callers gate via
// AuthThrottle first.
bool pamAuthenticateFull(const char* service, const char* user,
                         const char* password) noexcept;

} // namespace fbx::xserver
