#pragma once

namespace fbx::xserver {

// Try the xsessiond daemon (phase 1 of the unified seat/session daemon — see
// docs/xsessiond-design.md): 1 success, 0 reject, -1 if the daemon isn't
// reachable (caller falls back to in-process PAM). Definition in
// xsessiond_client.cc.
int authViaXsessiond(const char* user, const char* password) noexcept;

// Full password check for `service`: tries xsessiond (root daemon), then
// in-process PAM (only works if the compositor runs as root, i.e. dev). The old
// setuid xauthcheck helper is gone: xsessiond superseded it, and an installed
// setuid helper was an unthrottled password oracle for any local user. The
// in-process path runs pam_acct_mgmt after pam_authenticate, so disabled/
// expired/locked accounts are rejected even with a correct password. Shared by
// the login greeter and the screen locker so both enforce the same policy.
// Definition in login_service.cc. Does NOT throttle — callers gate via
// AuthThrottle first.
bool pamAuthenticateFull(const char* service, const char* user,
                         const char* password) noexcept;

} // namespace fbx::xserver
