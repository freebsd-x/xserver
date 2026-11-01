# Daemons

Standalone processes that ship with the compositor. They live in this repo, not
`src/`, because they share its wire protocols and headers (`include/xserver/`)
and must change in lockstep with it — but each one is its own **privilege
boundary**, so each has its own directory, and changes here deserve a closer
review than compositor code.

| Daemon | Runs as | Started by | Trusts | Job |
|---|---|---|---|---|
| [`xsessiond/`](xsessiond/) | **root** | rc.d (`xsessiond_enable`) | only connections from an allowed compositor executable (`XSESSIOND_COMPOSITOR`) | PAM authentication for login/unlock; brokers device fds (DRM, evdev) to the compositor over `SCM_RIGHTS`. Design: [`docs/xsessiond-design.md`](../docs/xsessiond-design.md) |
| [`xfetch/`](xfetch/) | the desktop user, **unsandboxed** | the compositor, at startup | only requests relayed by the compositor, and only to hosts in `fetch_policy.h` | HTTP(S) for Capsicum-sandboxed apps (`com.freebsdx.Fetch`) with a disk cache; replies with a read-only shared-memory fd. Never decodes what it fetches |

Rules of thumb:

- Keep daemons small and single-purpose; parsing untrusted data belongs in the
  sandboxed app, not here.
- A daemon that identifies its peers does it by **executable**
  (`peer_identity.h`), never by anything the peer says about itself.
- The compositor-side halves (`src/xsessiond_client.cc`, `src/fetch_broker.cc`)
  stay in `src/`; only the separate processes live here.
