# xsessiond — unified seat + session daemon (design sketch)

Status: **phase 1 landed (auth + session accounting; daemon-side session *spawn*
deferred); phase 2a landed (device-broker transport); phase 2b Stages A + B
landed but dormant (activation wire + libseat-underneath daemon + broker
selection, all behind default-off flags — compile/headless verified, live cutover
pending, see §14).** By default VT switching + seat activation still delegate to
external `seatd` via `libseat` (`src/seat_manager.cc`); the xsessiond broker path
(`XSESSIOND_USE_BROKER` + daemon `XSESSIOND_LIBSEAT`) is a gated alternative not
yet selected on the live seat.

Landed so far:

**Phase 1a — hosting the PAM check in the daemon:**
- `include/xserver/sessiond_protocol.h` — versioned SEQPACKET wire protocol.
- `src/xsessiond.cc` — the root daemon: single-process `poll()` event loop
  (one `Session` per connection fd), PAM (`pam_authenticate` + `pam_acct_mgmt`),
  env-scrubbed. Target `xsessiond`. (Originally accept-fork; converted to a
  single-process event loop as phase-2b groundwork — see the 2b note below.)
- `src/sessiond_client.cc` — `authViaSessiond()` (1/0/-1 contract, same as
  `authViaHelper`). `LoginService` and `LockService` try seatd → setuid
  helper → in-process PAM, in that order.
- `rc.d/xsessiond` — starts the daemon (root) `BEFORE: freebsdx`.

**Phase 1b — session lifecycle/accounting (connection = session):**
- Protocol adds `StartSession{leader_pid}` / `EndSession` → `SessionResult`,
  gated on a prior successful `Authenticate` on the same connection. The daemon
  child holds the PAM handle open, then on StartSession does
  `pam_setcred(ESTABLISH)` + `pam_open_session` + a **utmpx** USER_PROCESS
  record; on EndSession/disconnect it writes DEAD_PROCESS + `pam_close_session`
  + `pam_setcred(DELETE)`. So `who`/`w`/`last` finally see the GUI login, and a
  crashed compositor is still logged out (connection close = logout).
- `src/sessiond_client.cc` `SessiondSession::open(user, pass, leader)` — a live
  session held open for the desktop's lifetime; `LoginService` opens one
  **best-effort** on login success (`active_session_`) and keeps it alive.
  Additive: if the daemon is down or the session can't open, login proceeds
  exactly as before — the xshell spawn itself is untouched.
- Headless smoke test passes: Hello advertises auth+session caps, StartSession
  is refused without prior auth, reject paths + client `SessiondSession` return
  correctly. Positive session-open (real user, PAM session + utmpx) needs the
  root daemon on a live seat.

**Deferred — the greetd inversion (§4d full spawn):** having the daemon *exec*
the session as an arbitrary uid (`setusercontext` + drop privileges) so PAM
`limits`/`env`/credentials actually apply to the session processes. That
requires `freebsdx` to run privileged and can't be tested off the live seat, so
today the compositor still spawns xshell (as its own single user) and the daemon
only owns the accounting. This is the last piece of phase 1.

**Phase 2a — device-broker transport (fd passing over SCM_RIGHTS):**
- Protocol adds `OpenDevice{path}` → `Device{id}` **with the fd in the ancillary
  SCM_RIGHTS buffer** (result ≥ 0 is the device id, result < 0 a negative
  errno), and `CloseDevice{id}` → `DeviceClosed`. New `kCapDevice` bit in the
  `Welcome` caps.
- Daemon (`src/xsessiond.cc`): `device_open()` `realpath()`s the request and
  requires it to resolve under an allowlist (`/dev/drm/`, `/dev/dri/`,
  `/dev/input/`; char-device only) so the root daemon can't be coaxed into
  handing out arbitrary files; `send_device_fd()` passes the fd via
  `sendmsg`+`SCM_RIGHTS`. **FreeBSD device-layout note:** the real DRM nodes are
  `/dev/drm/N` and `/dev/dri/cardN` are compat *symlinks* into them — since the
  allowlist check runs *after* `realpath()`, `/dev/drm/` MUST be on the list or
  every DRM open resolves out of bounds and is refused with `EACCES` (this was the
  bug that made the broker path unusable on FreeBSD until it was added).
  Devices are leased per connection (`Session::devices`) and closed on
  `CloseDevice` **or** connection teardown. A test/dev override
  `XSESSIOND_DEV_PREFIX` (read before env scrub) replaces the allowlist and
  relaxes the char-device check so the fd path is headless-testable against a
  scratch file.
- Client: `SessiondDeviceManager` (`include/xserver/sessiond_device.h`,
  `src/sessiond_device.cc`) implements the existing `ISeatManager` interface,
  receiving the fd with `recvmsg`+`MSG_CMSG_CLOEXEC`. It is a **gated, opt-in
  alternative** — the live compositor still constructs the libseat-backed
  `SeatManager`; nothing selects `SessiondDeviceManager` yet.
- Headless smoke test passes as uid 1001: caps advertise `kCapDevice`, an fd is
  brokered and is a live independent descriptor (write through it lands in the
  underlying file), device-id round-trip, `CloseDevice` + unknown-id refusal,
  and an out-of-allowlist path (`/etc/master.passwd`) is refused.
- **Live-verified on real hardware (2026-10-02, FreeBSD 15.1 / Intel i915):** with
  the `/dev/drm/` allowlist fix, the broker hands the compositor a real DRM fd —
  `freebsdx` with `XSESSIOND_USE_BROKER=1` logged `Hardware Found: /dev/dri/card0
  (Master lock acquired)`, probed the connector/CRTC, IGNITED output 0 (1920×1080),
  and brought up the full desktop (greeter + xshell rendering) entirely on a
  brokered fd, then tore down cleanly (console + master handed back). The daemon
  ran unprivileged (uid 1001 is in `video`, so it can `open()` `/dev/drm/0`). This
  is phase 2a **transport** proven end-to-end; it is *not* phase 2b — VT-switch
  activation (`SeatDisable`/`SeatEnable`) is still inert without `XSESSIOND_LIBSEAT`
  (which needs real seatd underneath), so this one-compositor path works but VT
  switching away/back is still unhandled. Input is not brokered through xsessiond
  (the compositor's `InputManager` still opens evdev via its own libseat path).

**Phase 2b groundwork — single-process event loop (landed, ahead of the live
work):** the daemon was converted from accept-fork (one child per connection) to
a single-process `poll()` loop holding one `Session` per connection fd
(`std::unordered_map<int,Session>` — node addresses stay stable, which matters
because PAM stashes `&Session` as its conv appdata). This is a prerequisite for
2b: a single process must own the seat + the *set* of active sessions to
drop/reacquire DRM master across them on a VT switch — isolated per-connection
children can't. Headless-tested: 4 concurrent connections each answered by the
one process, plus the existing auth/session + device brokering smoke tests, with
the daemon surviving many connections. No behaviour change to the wire protocol.
Note for the live work: session-module `pam_setcred`/`pam_open_session` now run
in the shared daemon process rather than a private child — a no-op for the
current `freebsdx` PAM stack, but confirm on the seat, and phase 1c's session
*spawn* will fork+exec (so credential establishment lands in the session child,
not the daemon).

**Deferred — phase 2b (seat activation / libseat-underneath):** the remaining,
live-seat-only piece that makes `SessiondDeviceManager` safe to *actually select*.
The (now single-process) daemon must become a `libseat` client of real `seatd`
so that on VT switch it drops/reacquires DRM master and forwards
`Enable`/`Disable` events to the compositor — today `getFd()`/`dispatch()` on the
client are stubs. Opening a DRM node directly without that coordination would
break VT switching, so this can only be built and validated on the live seat.
Until then phase 2a is transport-only.

## 1. Motivation

Seat/session responsibility is currently split across four pieces, three of
which fbx already owns:

| Concern | Today | Owner |
| --- | --- | --- |
| Open DRM/input nodes, pass fds, VT switching | external `seatd` via `libseat` | seatd |
| Who is logged in / which session is active | *nobody* (implicit, single session) | — |
| Greeter spawn, session (xshell) spawn | `LoginService` | fbx |
| PAM password check | `xauthcheck` (setuid helper) | fbx |

seatd owns only the **device/VT half**. fbx already owns the **login/session
half**. A unified `xsessiond` would merge both behind the API seam we already
have (`libseat`), so the platform has one privileged daemon that knows the full
truth: which seats exist, which session is active on each, who is authenticated,
and which process holds the devices.

This is the *only* reason to build it — integration, not functionality. If that
milestone never comes, keeping `seatd` is correct (see §11).

## 2. Goals / non-goals

**Goals**
- One small privileged daemon; the compositor and all UI stay unprivileged.
- Absorb seatd's device brokering + VT switching, logind-lite session tracking,
  `LoginService`'s spawn role, and `xauthcheck`'s PAM check.
- Preserve the `libseat` client API so `SeatManager` needs **zero** changes
  (compat backend, §11), while adding a richer native protocol for the
  session/login features libseat doesn't cover.
- Support fast-user-switching and lock/greeter-on-another-VT as first-class.

**Non-goals**
- Multi-*seat* (multiple GPUs/keyboards → independent seats) beyond leaving room
  for it in the protocol. Seat0-only is fine for v1.
- Replacing PAM. We still call PAM; we just host the call.
- Network/remote sessions.
- Being a drop-in for logind's D-Bus API (we expose our own protocol).

## 3. Process & privilege model

```
          root
        ┌──────────────────────────────────────────┐
        │  xsessiond (PID 1-spawned or rc.d)        │
        │  - opens /dev/dri/*, /dev/input/* (evdev) │
        │  - owns the VTs, does VT switching        │
        │  - runs PAM (pam_authenticate/acct_mgmt)  │
        │  - tracks seats + sessions                │
        │  - spawns sessions as the target uid      │
        └───────────────┬──────────────────────────┘
                        │ AF_UNIX SOCK_SEQPACKET, SCM_RIGHTS
        ┌───────────────┴───────────────┐
        │ freebsdx (compositor, unpriv) │   ← one per active session/VT
        │  SeatManager (libseat client) │
        │  LoginService (client of seatd│
        │   session API, no more fork)  │
        └───────────────────────────────┘
```

- Runs as **root**, dropping to minimal capabilities where FreeBSD allows
  (Capsicum is hard here because it needs `open()` on device nodes on demand;
  more realistically: careful, tiny, audited, no untrusted parsing beyond the
  framed protocol).
- The compositor never opens a privileged node and never touches VT ioctls.
- Transport: **AF_UNIX `SOCK_SEQPACKet`** — matches the IPC transport the
  project already moved to (framed, no coalescing), and gives clean `SCM_RIGHTS`
  fd passing without the unframed-stream pitfalls documented elsewhere.

## 4. Responsibilities absorbed

### 4a. Device brokering (was: seatd)
- `OpenDevice(path)` → validates the path is a real DRM or evdev node on a seat
  the caller owns, `open()`s it, returns the fd via `SCM_RIGHTS` + a device id.
- `CloseDevice(id)`.
- On session deactivate (VT switch away): drop DRM master on all the session's
  DRM fds, `EVIOCREVOKE` (or close) input fds, tell the client via an event.
- On activate: reacquire DRM master, reopen/re-grant input, event the client.

### 4b. VT / seat switching (was: seatd)
- Owns `VT_SETMODE(VT_PROCESS)` on the console, handles the acquire/release
  signal handshake, and arbitrates `VT_ACTIVATE`.
- Exposes `SwitchToSession(id)` and `SwitchToVT(n)` to authorized clients.

### 4c. Session tracking (new; logind-lite)
- Concepts: **Seat** (a set of devices + a VT range) and **Session** (a uid + a
  VT + a compositor process + a set of leased devices + active/inactive).
- Tracks the active session per seat; drives 4a/4b on switch.

### 4d. Session/login orchestration (was: LoginService)
- At boot: create the greeter session (a `freebsdx` running the greeter) on
  seat0's first VT.
- `Authenticate(user, secret)` → runs PAM in-process (root, so it can read
  `master.passwd`) → on success, `StartSession(user)`: set up the utmpx entry,
  `setusercontext`, `initgroups`, drop to the uid, `exec` the session compositor.
- `LockSession` / `UnlockSession(secret)` — the locker calls `Authenticate`
  against the *owning* uid rather than spawning anything.

### 4e. Auth (was: xauthcheck)
- The PAM check moves *into* xsessiond (already root, no separate setuid binary
  needed). `xauthcheck` can be retired, OR kept as the fallback for
  configurations that run without xsessiond. `authViaHelper` becomes
  `authViaSessiond` — same 1/0/-1 contract, so `LoginService`/`LockService`
  callers barely change.

## 5. Protocol (native, versioned)

Framed request/response over SEQPACKET; every message has `{version, opcode,
serial}`. fds ride in the ancillary buffer.

```
Client → daemon
  Hello{version}                         → Welcome{seat_id, session_id, caps}
  OpenDevice{path}                       → Device{id} + fd
  CloseDevice{id}                        → Ok
  Authenticate{user, secret}             → AuthResult{ok, reason}
  StartSession{user}                     → SessionStarted{session_id} (daemon execs compositor)
  SwitchToSession{id} / SwitchToVT{n}    → Ok
  LockSession{} / Unlock{secret}         → Ok / AuthResult
  ReleaseSession{}                       → Ok

Daemon → client (events, unsolicited)
  Enable{}      // your session is now active: reacquire devices
  Disable{}     // your session is going inactive: drop DRM master NOW, ack
  DisableAck required within Nms or daemon force-revokes
  DeviceRevoked{id}
  SessionEnded{}
```

`Enable`/`Disable` mirror libseat's `enable_seat`/`disable_seat` exactly (§11) —
`SeatManager::onEnableSeat/onDisableSeat` map 1:1.

## 6. VT switching state machine (per seat)

```
        ┌─────────┐  SwitchTo(target)   ┌──────────────┐
        │ ACTIVE  │ ───────────────────▶│ RELEASING    │
        │ (sess A)│                     │ send Disable │
        └─────────┘                     │ to A, wait   │
             ▲                          │ DisableAck   │
             │ EnableAck from B         └──────┬───────┘
        ┌────┴──────┐                          │ ack / timeout
        │ ACQUIRING │◀── VT_RELDISP + ─────────┘
        │ send      │    VT_ACTIVATE(targetVT)
        │ Enable B  │
        └───────────┘
```

FreeBSD specifics (vt(4)/syscons):
- `VT_SETMODE{ mode=VT_PROCESS, relsig=SIGUSR1, acqsig=SIGUSR2 }` on the console fd.
- On `relsig`: drop DRM master on the outgoing session's fds, revoke its input,
  then `VT_RELDISP(VT_TRUE)` to allow the switch.
- On `acqsig`: `VT_RELDISP(VT_ACKACQ)`, then reacquire master/input for the new
  session and send it `Enable`.
- Query/activate: `VT_OPENQRY`, `VT_GETACTIVE`, `VT_ACTIVATE`, `VT_WAITACTIVE`.
- The signal handshake runs on a dedicated fd/thread or a self-pipe integrated
  into the daemon's poll loop (no async-signal-unsafe work in the handler).

**This is the single most dangerous subsystem** — a bug can wedge every VT and
lock the machine. It is also the only part `seatd` already gets right. See §12.

## 7. Login / session lifecycle

```
boot → xsessiond starts (root)
     → creates seat0, greeter session on VT1, execs `freebsdx --greeter` (unpriv)
greeter → Authenticate{user,secret} → PAM (root) → AuthResult
        → StartSession{user} → daemon: utmpx, setusercontext, initgroups,
          setgid/setuid(user), exec `freebsdx` (the user session) on VT1
        → greeter session ends (or parks on another VT for fast-switch)
lock    → compositor: LockSession → later Unlock{secret} → Authenticate(owner)
switch user → new greeter session on VT2, SwitchToVT(2); original stays live on VT1
```

`LoginService` shrinks to a *client* of this API: it no longer forks the
greeter, sets uids, or calls PAM — it asks xsessiond to. This deletes the
riskiest privileged code paths from the compositor process.

## 8. Security considerations

- **Attack surface = the protocol parser + PAM + exec path.** Keep the parser
  tiny and total; reject any device path not under an allowlist of real
  DRM/evdev nodes on the caller's seat.
- **Authorization:** a client may only operate on *its own* session; only the
  active session (or a privileged installer) may `SwitchTo*`. Identify clients by
  the connected socket's `SO_PEERCRED`/`getpeereid` (uid), not by anything in the
  message.
- **Secret handling:** password bytes arrive over the SEQPACKET socket (not
  argv/env), are fed straight to PAM, and are zeroed after. Same discipline as
  `xauthcheck` today.
- **No env trust in the exec path** (the setuid-hygiene lesson from
  `authcheck.cc`): build the child env explicitly.
- **Denial of service:** cap in-flight requests per client; `DisableAck` timeout
  bounds a misbehaving client's ability to stall a VT switch.

## 9. libseat compatibility & migration (§11 detail)

xsessiond ships a **libseat backend** (or speaks seatd's wire protocol on a
compatible socket) so `libseat_open_seat()` in `SeatManager` keeps working with
no code change. The native session/login opcodes (§5) are an *additive* channel
used by `LoginService`/`LockService`, not by the compositor's device path.

Result: the compositor is backend-agnostic — real `seatd`, `logind`, or
`xsessiond` all satisfy `SeatManager`. That's the seam that makes this safe to
attempt and safe to abandon.

## 10. Phased plan (each phase independently shippable)

Each phase splits into a headless-testable slice (built here, off the seat) and
a live-seat-only slice — the same discipline the work has followed: land and
prove the mechanism headlessly, defer the part that can only be validated on the
live seat.

1. **Session API only, seatd underneath.** Build xsessiond as a *session*
   daemon (login/spawn/auth/lock, §4c–e) that itself is a `libseat` *client* for
   devices. Zero VT/DRM risk. Moves PAM + spawn out of the compositor. Highest
   value, lowest risk — do this first, maybe *instead* of the rest.
   - **1a — auth (done).** Root daemon hosts the PAM check
     (`pam_authenticate` + `pam_acct_mgmt`) over SEQPACKET; `authViaSessiond`
     joins the seatd → setuid-helper → in-process-PAM chain.
   - **1b — session accounting (done).** `StartSession`/`EndSession` open a PAM
     session + write utmpx (connection = session); `SessiondSession` holds it for
     the desktop's lifetime, best-effort. Positive path needs the root daemon
     on a live seat.
   - **1c — greetd inversion (deferred, §4d).** Daemon *execs* the session as an
     arbitrary uid so PAM limits/env/creds apply to session processes. Needs
     `freebsdx` privileged; live-seat-only.

2. **libseat compat: device brokering.** Run the compositor against xsessiond
   for `OpenDevice` while VT switching still delegates to seatd internally.
   - **2a — device-broker transport (done).** `OpenDevice`/`CloseDevice`
     opcodes; the daemon `realpath()`s + allowlists the node and passes the fd
     over `SCM_RIGHTS`; client `SessiondDeviceManager` satisfies the existing
     `ISeatManager` interface. Headless-testable end to end (fd passing against
     a scratch file). **Gated/opt-in — not selected by the live compositor.**
   - **2b — seat activation / libseat-underneath (Stages A + B landed, dormant;
     Stage C live cutover pending).**
     *Groundwork:* single-process `poll()` event loop (was accept-fork) — one
     process must own the seat + active-session set to coordinate DRM master.
     *Stage A (landed):* activation wire — unsolicited `SeatDisable`/`SeatEnable`
     push replies + `DisableAck` + `kCapActivation`; `SessiondDeviceManager::
     dispatch()` drains them, invokes handlers, and answers DisableAck *after*
     the compositor quiesces (ordering = the safety-critical bit). Headless-
     tested over a socketpair.
     *Stage B (landed, behind default-off flags):* daemon `XSESSIOND_LIBSEAT`
     makes it a `libseat` client of real seatd (brokered, properly-mastered fds;
     relays enable/disable; waits all DisableAcks before `libseat_disable_seat`).
     Compositor `XSESSIOND_USE_BROKER` selects `SessiondDeviceManager` over the
     libseat `SeatManager` and gates rendering on `g_seat_active`. Default build
     + behavior unchanged; daemon libseat path is compile-verified only.
     *Stage C (pending, live-seat-only):* flip both flags on the seat and validate
     a real VT switch, with a recovery path — **runbook in §14.**

3. **Native VT switching.** Replace the internal seatd dependency with the §6
   state machine. Gated behind a config flag; A/B against seatd. **Test only on a
   disposable machine.**
4. **Retire seatd + xauthcheck** once phase 3 is proven on hardware.

Stopping after phase 1 is a perfectly good outcome; so is stopping after 2a with
device brokering proven but not yet cut over.

## 11. Why not just keep seatd (the counter-argument, recorded)

- seatd is tiny, battle-tested by the whole wlroots/sway ecosystem, and already
  packaged on FreeBSD.
- The VT/DRM-master handshake is the exact code most likely to lock you out, and
  it is the part seatd already gets right.
- Our own constraint — *never run freebsdx non-headless from the dev shell, it
  freezes the seat* — means phase 3 cannot be iterated here at all.
- The device-brokering half gains us nothing by being rewritten. **Only the
  session/login integration (phase 1) has real payoff.**

Recommendation: if this is ever picked up, do **phase 1** and likely stop there,
keeping seatd as the libseat backend for devices/VTs.

## 12. Testing strategy

- Headless-testable slices (1a, 1b, 2a): the daemon on a scratch socket + a
  mock client over SEQPACKET, `XSESSIOND_DEV_PREFIX` to broker a scratch file so
  the SCM_RIGHTS path runs without a real `/dev` node. Fits the existing
  headless flow; positive PAM auth / `pam_open_session` / utmpx writes still
  need root on a live seat (uid 1001 can only exercise the reject + protocol
  paths).
- Live-seat-only slices (1c greetd inversion, 2b seat activation): can't be
  validated off the seat — 1c needs `freebsdx` privileged; 2b needs the daemon
  to hold real DRM master and drive the VT handshake. Build and prove these on
  the seat, not from the dev shell.
- Phase 3 (VT switching): **not** testable from the live seat. Requires a
  dedicated FreeBSD box or VM with serial console access so a wedged VT is
  recoverable, plus a hardware watchdog / remote reset. Do not attempt on the
  development machine.

## 13. Open questions

- Capsicum: can any part be sandboxed, or does on-demand device `open()` force
  full privilege? (Likely the latter for the device broker; the session/auth
  logic could potentially be a separate capability-mode worker.)
- utmpx/wtmp accounting parity with `login(1)` — which fields does xshell rely
  on?
- Fast-user-switching resource model: keep N idle compositors alive on parked
  VTs, or freeze/serialize inactive sessions?
- Do we want a D-Bus/XBus bridge so non-privileged apps can query "who's logged
  in" without speaking the raw protocol?
```

## 14. Phase 2b Stage C — live-cutover runbook

Stages A (activation wire + client dispatch) and B (libseat-underneath daemon +
`XSESSIOND_USE_BROKER` compositor selection) are landed but **dormant**: both
paths are off unless their env flags are set, and the daemon's libseat path is
compile-verified only. Stage C is the first time either runs for real. It hands
DRM master around on a live VT switch — **a bug here can black out or wedge every
VT**. seatd is still the safety net underneath (this is *not* native VT
switching, which is phase 3), but treat it as dangerous.

### Preconditions (do not skip)
- **A second way in.** Either a disposable/spare machine, or SSH from another box
  so you can kill a wedged compositor without the local console. Confirm the SSH
  session works *before* cutting over.
- Know the recovery commands cold (below). Have them pasted and ready.
- Ideally test on a disposable machine first; only then the daily driver.

### Why it can't be done from inside the running GUI
The current compositor holds seatd's *active* session for the seat. If xsessiond
opens its own libseat seat while that session is active, seatd keeps xsessiond
*inactive* — its `enable_seat` never fires and the priming loop exits with "seat
never activated". So the running GUI must be stopped first; the cutover is done
from a text VT or over SSH.

### Cutover sequence
1. **Stop the GUI** so seatd's active session is free:
   `sudo service <compositor-rc> stop` (or kill the running `freebsdx`). Confirm
   `fuser /dev/dri/card0` is empty.
2. **Run xsessiond in libseat mode** (foreground, so you see its log). Stop the
   installed service first to free the socket:
   ```sh
   sudo service xsessiond stop
   sudo env XSESSIOND_LIBSEAT=1 /usr/local/bin/xsessiond
   ```
   Expect `libseat-client mode active (seat brokered)`. If it prints "seat never
   activated", something else still holds the active session — go back to step 1.
3. **Launch the compositor against the broker** from a *second* SSH/VT:
   ```sh
   sudo env XSESSIOND_USE_BROKER=1 <launch freebsdx as usual, on the seat VT>
   ```
   Expect `[seat] device broker: xsessiond (XSESSIOND_USE_BROKER)` and a normal
   display. If the broker is unreachable it logs a warning and falls back to
   libseat — check the daemon is up.

### Validate on the seat
- Display comes up, input works, screen isn't blinking (flip-on-dirty still holds).
- **VT switch away** (Ctrl-Alt-F2): daemon logs a disable, compositor logs
  `SeatDisable — GPU quiesced`; the text console appears cleanly (master handed
  back). Nothing spins the GPU.
- **VT switch back** (Ctrl-Alt-F1): compositor logs `SeatEnable — reacquired
  seat, repainting` and the desktop returns intact. Do this 5–10× rapidly to
  shake out ordering races (the DisableAck-before-`libseat_disable_seat`
  handshake is the risky part).
- Confirm `who`/`last` still track the session (Stage-1b accounting unaffected).

### Recovery (rehearse first)
- **Compositor wedged, display black:** from SSH — `sudo pkill -x freebsdx`;
  seatd/xsessiond should restore the VT. Then `sudo pkill -x xsessiond` if needed.
- **VT stuck in graphics mode:** `sudo vidcontrol -s 1 < /dev/ttyv0`, or switch
  with `sudo vidcontrol` on another VT; last resort `sudo shutdown -r now`.
- **Restore the known-good path:** just relaunch without the env flags — no
  reinstall needed, both flags default off. Re-enable the normal service with
  `sudo service xsessiond start`.

### Making it permanent (only after it's proven)
Once VT switching is solid across many cycles: bake `XSESSIOND_LIBSEAT=1` into the
rc.d script's `command_args` env and `XSESSIOND_USE_BROKER=1` into the compositor
launch, then re-run `scripts/xsessiond-install.sh`. Keep the ability to boot
without them (a loader/rc toggle) until phase 3.

## 15. Launch configuration & the device-broker keystroke leak (2026-10)

Bringing the phase-2a broker up live on FreeBSD (Intel i915) turned up two bugs
and one hard limitation that together decide how the compositor should launch.

### Bugs fixed on the broker path
- **`/dev/drm/` allowlist** (`xsessiond.cc`, commit `2358e46`): FreeBSD's real DRM
  nodes are `/dev/drm/N` with `/dev/dri/cardN` as compat symlinks; `device_resolve()`
  `realpath()`s before the allowlist check, so `/dev/dri/card0` → `/dev/drm/0` was
  refused `EACCES`. Added `/dev/drm/` to the default allowlist. Without this the
  broker hands out *nothing* on FreeBSD.
- **`O_NONBLOCK` on brokered fds** (`xsessiond.cc`, commit `2f55b89`): `device_open()`
  handed out blocking fds; libinput needs evdev non-blocking and hangs
  `libinput_udev_assign_seat()` on a blocking fd, stalling compositor startup right
  after DRM output assignment. `device_open()` now opens
  `O_RDWR|O_CLOEXEC|O_NONBLOCK` (matches seatd/libseat). Masked by the uid-1001 test
  daemon (couldn't open the `0600` evdev nodes → libinput skipped input); only the
  root daemon exposed it.
- **`SIGPIPE` ignore** (`main.cc`, commit `7e91a18`): the compositor never ignored
  SIGPIPE, so at the greeter→desktop handoff (`startSession()` kills the greeter, a
  later write hits the closed socket) it died with status 141 — and SIGPIPE never
  dumps a core, which is why the crash left no trace. `signal(SIGPIPE, SIG_IGN)`
  now covers every client socket. Not broker-specific; any client disconnect could
  have triggered it.

### The limitation: phase 2a does not grab the VT
The broker brokers **devices** (DRM + evdev, verified live: GPU + keyboard/mouse/
touchpad, greeter renders, login works) but does **no VT control**. On a console
the kernel still delivers keystrokes to the active VT's tty, so a user typing their
login at the greeter *also* types into whatever shell owns that VT — a real
credential leak. Confirmed with a `KDGKBMODE` probe: under the broker the active VT
stays `K_XLATE` (translated → leaks).

### Resolution — default to seatd, broker is opt-in
Real `seatd` is installed here (`seatd-0.9.3`), and the leak-free fix is simply to
run the compositor on the **default libseat path under seatd** (broker *off*): seatd
grabs the session VT into `K_RAW`, so console keystrokes stop reaching the tty.
**Verified on the physical console**: launched from the active VT (ttyv0), all
`KDGKBMODE` samples read `K_RAW` for the duration and restored to `K_XLATE` on exit;
a real login did not leak. Key gotcha: seatd grabs the compositor's *session* VT
(the one it is launched from), so the faithful test must launch from the active
console — a backgrounded launch grabs the wrong VT and reads a false `K_XLATE`.

So the launch config (in `scripts/startx` and the `freebsdx_env` of `rc.d/freebsdx`;
both on-disk/untracked like the rest of `rc.d/`) **defaults to the plain
libseat→seatd path with no broker flag**. `XSESSIOND_USE_BROKER=1` is **opt-in**,
documented inline in both files as devices-only / leaks-on-console. `seatd` and
`xsessiond` are both enabled at boot — `xsessiond` still hosts **auth + session
accounting** (`authViaSessiond`, utmpx; `last` shows real `xsess0` logins) even
though devices go through seatd on the default path.

### Health check (how to confirm xsessiond is working)
- `meson test -C out "xsessiond smoke"` — real daemon over a scratch socket (caps,
  concurrency, auth-gating, device+fd brokering).
- Live: `Hello`→`Welcome` caps; `Authenticate` with a **non-existent** user →
  `AuthResult=0` (proves the PAM path responds without touching a real account);
  `OpenDevice /dev/dri/card0` → fd, `OpenDevice /etc/master.passwd` → refused.
- Positive auth/session: `last` shows `wheel  xsess0` entries for real greeter
  logins (utmpx via `StartSession`).

### Still the frontier
Getting the broker **and** no-leak together (so xsessiond owns the whole stack) is
**Stage C** above: `xsessiond XSESSIOND_LIBSEAT=1` as a libseat client of seatd,
compositor with `XSESSIOND_USE_BROKER=1`. Unchanged from §14 — still gated behind a
confirmed SSH escape hatch; a VT-grab test from the bare console risks a keyboard
lockout (reboot) with no second way in.

### Operational: what the default `startx` path needs at boot
The default path is libseat → **real seatd**, so **seatd must be running** for
`scripts/startx` to acquire the GPU. `xsessiond` has its own rc.d (`xsessiond_enable`)
and comes back on reboot for auth/session, but **seatd is a separate package daemon**
— enable it too or `startx` fails to get the display after a reboot:
```sh
sudo sysrc seatd_enable=YES
sudo service seatd start
```
Pre-flight for a clean launch: `pgrep -x seatd` + `/var/run/seatd.sock` present, no
stale `freebsdx`/`xshell`, and `$WAYLAND_DISPLAY`/`$DISPLAY` empty (startx refuses
otherwise). Exit the running stack with **Ctrl+Alt+Esc** (compositor's emergency
shutdown) or `Ctrl+C` in the startx terminal — both do a clean teardown that
restores the console CRTC, drops DRM master, and hands the VT back to seatd
(keyboard returns to `K_XLATE`).

### Verified end-to-end on the live seat (2026-10-05)
A full real login was validated from the physical console on the default seatd path:
`Master lock acquired` → `libinput via seatd` → output `IGNITED` → greeter →
`Authentication succeeded for user: wheel` → `SPAWNING xshell` → desktop rendered
(40 draw commands, live AC-power events) → clean `Ctrl+Alt+Esc` teardown. **No
keystroke leak** (seatd's VT grab; the login did not echo to the console shell),
**no handoff crash** (needed the SIGPIPE-ignore fix, `7e91a18` — the greeter→desktop
handoff writes to the just-closed greeter socket), and **no compositor freeze**
(needed the popen/system→posix_spawn sweep, `60384da`+`67d0d2e` — shell-outs on the
event-loop/background threads otherwise fork a child that deadlocks on a libc stdio
lock and wedges IPC). These three fixes are what make the default launch path usable
end to end; a regression in any of them reintroduces a leak, a handoff crash, or a
freeze respectively.
