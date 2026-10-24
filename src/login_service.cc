#include "xserver/login_service.h"
#include "xserver/auth_helper.h"
#include "xserver/auth_throttle.h"
#include "xserver/security_events_service.h"
#include "xserver/xsessiond_session.h"
#include "xserver/ipc_socket.h"
#include <security/pam_appl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <string>
#include <strings.h>
#include <vector>
#include <thread>
#include <chrono>

namespace fbx::xserver {

std::atomic<bool>           LoginService::greeter_active_{false};
std::mutex                  LoginService::state_mutex_;
std::weak_ptr<IpcSocket>    LoginService::greeter_client_;
pid_t                       LoginService::greeter_pid_ = -1;
std::function<void()>       LoginService::shell_spawner_;
std::unique_ptr<XsessiondSession> LoginService::active_session_;

void LoginService::setShellSpawner(std::function<void()> fn) noexcept {
    shell_spawner_ = std::move(fn);
}

// ── PAM ──────────────────────────────────────────────────────────────────────

struct PamData { const char* password; };

static int pam_conv_fn(int num_msg, const struct pam_message** msgs,
                       struct pam_response** resps, void* appdata) {
    auto* pd = static_cast<PamData*>(appdata);
    auto* r = static_cast<struct pam_response*>(
        calloc(static_cast<size_t>(num_msg), sizeof(struct pam_response)));
    if (!r) return PAM_BUF_ERR;
    for (int i = 0; i < num_msg; ++i) {
        if (msgs[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
            msgs[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            r[i].resp = strdup(pd->password);
        }
    }
    *resps = r;
    return PAM_SUCCESS;
}

// Try the setuid-root xauthcheck helper. Returns 1 on auth success, 0 on
// auth failure, -1 if the helper is unavailable (so the caller can fall back to
// in-process PAM, which works when the compositor itself runs as root).
// Declared in xserver/auth_helper.h; also used by LockService.
int authViaHelper(const char* user, const char* password) noexcept {
    const char* candidates[] = {
        "./out/xauthcheck", "./xauthcheck",
        "/usr/local/libexec/xauthcheck", "/usr/local/bin/xauthcheck",
    };
    // Only use a candidate that is actually setuid root — otherwise it would run
    // with our own (unprivileged) uid and couldn't read the shadow DB anyway. A
    // plain build-dir copy (not yet `meson install`ed) is skipped, and if none
    // qualify we return -1 so the caller falls back to in-process PAM (which
    // works when the compositor itself is run as root).
    const char* path = nullptr;
    for (const char* c : candidates) {
        struct stat st;
        if (::stat(c, &st) == 0 && st.st_uid == 0 && (st.st_mode & S_ISUID) &&
            ::access(c, X_OK) == 0) {
            path = c;
            break;
        }
    }
    if (!path) return -1;

    int in_pipe[2];
    if (::pipe(in_pipe) != 0) return -1;
    pid_t pid = ::fork();
    if (pid < 0) { ::close(in_pipe[0]); ::close(in_pipe[1]); return -1; }
    if (pid == 0) {
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::execl(path, path, static_cast<char*>(nullptr));
        _exit(127);  // exec failed
    }
    ::close(in_pipe[0]);
    std::string payload = std::string(user) + "\n" + password + "\n";
    size_t off = 0;
    while (off < payload.size()) {
        ssize_t w = ::write(in_pipe[1], payload.data() + off, payload.size() - off);
        if (w <= 0) break;
        off += static_cast<size_t>(w);
    }
    // Scrub the plaintext password from our heap before the buffer is freed —
    // explicit_bzero isn't elided by the optimizer like memset can be.
    ::explicit_bzero(payload.data(), payload.size());
    ::close(in_pipe[1]);
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 127) return -1;   // helper couldn't exec → fall back
        return code == 0 ? 1 : 0;
    }
    return 0;
}

// Shared full-chain password check (declared in auth_helper.h). Both the login
// greeter and the screen locker route through this so they enforce identical
// policy — notably pam_acct_mgmt on the in-process fallback, which login's old
// inline path skipped (a disabled/expired account could have logged in).
bool pamAuthenticateFull(const char* service, const char* user,
                         const char* password) noexcept {
    // Preferred: the xsessiond daemon (does PAM as root). Then the setuid-root
    // helper. Both keep the compositor unprivileged; only fall back to
    // in-process PAM (below) if neither is available (i.e. we run as root).
    if (int s = authViaXsessiond(user, password); s >= 0) return s == 1;
    if (int h = authViaHelper(user, password); h >= 0) return h == 1;

    PamData pd{password};
    struct pam_conv conv{pam_conv_fn, &pd};
    pam_handle_t* pamh = nullptr;
    if (pam_start(service, user, &conv, &pamh) != PAM_SUCCESS) return false;
    int ret = pam_authenticate(pamh, 0);
    if (ret == PAM_SUCCESS) ret = pam_acct_mgmt(pamh, 0);  // reject disabled/expired
    pam_end(pamh, ret);
    return ret == PAM_SUCCESS;
}

bool LoginService::pamAuthenticate(const char* user, const char* password) noexcept {
    return pamAuthenticateFull("freebsdx", user, password);
}

// ── User list from /etc/passwd ────────────────────────────────────────────────

void LoginService::listUsers(std::shared_ptr<IpcSocket> client) noexcept {
    using namespace shared;
    struct Entry { std::string username; std::string display; };
    std::vector<Entry> users;

    FILE* fp = fopen("/etc/passwd", "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            // username:x:uid:gid:gecos:home:shell
            char* saveptr = nullptr;
            char* uname = strtok_r(line,  ":", &saveptr);
            if (!uname) continue;
            strtok_r(nullptr, ":", &saveptr); // password field
            char* uid_s  = strtok_r(nullptr, ":", &saveptr);
            if (!uid_s) continue;
            int uid = atoi(uid_s);
            if (uid < 1000) continue;          // skip system accounts
            strtok_r(nullptr, ":", &saveptr);  // gid
            char* gecos = strtok_r(nullptr, ":", &saveptr);
            std::string display = uname;
            if (gecos && *gecos) {
                // Trim after first comma (GECOS format)
                char* comma = strchr(gecos, ',');
                if (comma) *comma = '\0';
                if (*gecos) display = gecos;
            }
            users.push_back({uname, display});
        }
        fclose(fp);
    }

    // Fallback: if no UID-≥1000 users found (minimal install), add current user
    if (users.empty()) {
        const char* u = getenv("USER");
        if (!u) u = "user";
        users.push_back({u, u});
    }

    uint32_t total = static_cast<uint32_t>(users.size());
    for (uint32_t i = 0; i < total; ++i) {
        XBusMessage resp{};
        resp.magic_ = XBUS_MAGIC;
        std::strncpy(resp.service_name_, "com.freebsdx.Login", sizeof(resp.service_name_) - 1);
        resp.opcode_ = static_cast<uint32_t>(LoginOpcode::UsersPage);
        LoginUsersPage pg{};
        pg.page_  = i;
        pg.total_ = total;
        pg.valid_ = 1;
        std::strncpy(pg.username_,     users[i].username.c_str(), sizeof(pg.username_) - 1);
        std::strncpy(pg.display_name_, users[i].display.c_str(),  sizeof(pg.display_name_) - 1);
        std::memcpy(resp.payload_, &pg, sizeof(pg));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
    }

    // Sentinel page: valid_=0 signals end-of-list
    XBusMessage end_resp{};
    end_resp.magic_ = XBUS_MAGIC;
    std::strncpy(end_resp.service_name_, "com.freebsdx.Login", sizeof(end_resp.service_name_) - 1);
    end_resp.opcode_ = static_cast<uint32_t>(LoginOpcode::UsersPage);
    LoginUsersPage end_pg{};
    end_pg.page_  = total;
    end_pg.total_ = total;
    end_pg.valid_ = 0;
    std::memcpy(end_resp.payload_, &end_pg, sizeof(end_pg));
    (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&end_resp), sizeof(end_resp)});
}

// ── Session start ─────────────────────────────────────────────────────────────

void LoginService::startSession(const char* username) noexcept {
    std::cout << "[LoginService] Authentication succeeded for user: " << username << "\n";
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        greeter_active_ = false;
        greeter_client_.reset();
    }
    if (greeter_pid_ > 0) {
        kill(greeter_pid_, SIGTERM);
        waitpid(greeter_pid_, nullptr, WNOHANG);
        greeter_pid_ = -1;
    }
    // Set USER env so xshell / apps see the authenticated user
    setenv("USER", username, 1);
    if (shell_spawner_) shell_spawner_();
}

// ── Spawn greeter ─────────────────────────────────────────────────────────────

void LoginService::spawnGreeter() noexcept {
    std::cout << "[LoginService] Spawning xlogin greeter...\n";
    greeter_active_ = true;
    greeter_pid_ = fork();
    if (greeter_pid_ == 0) {
        setenv("LD_LIBRARY_PATH", "./out", 1);
        setenv("XBUS_SOCKET", "/tmp/freebsdx.sock", 1);
        if (access("./xlogin", X_OK) == 0)            // dev build (builddir/)
            execl("./xlogin", "xlogin", nullptr);
        if (access("./out/xlogin", X_OK) == 0)
            execl("./out/xlogin", "xlogin", nullptr);
        if (access("/usr/local/bin/xlogin", X_OK) == 0)
            execl("/usr/local/bin/xlogin", "xlogin", nullptr);
        std::cerr << "[LoginService] FATAL: failed to exec xlogin\n";
        exit(1);
    }
    // If xlogin exits immediately (binary not found or crashes at startup),
    // skip straight to the shell — allows dev workflow without xlogin installed.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    int status = 0;
    if (waitpid(greeter_pid_, &status, WNOHANG) > 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        std::cout << "[LoginService] Greeter unavailable — auto-spawning shell\n";
        greeter_active_ = false;
        greeter_pid_ = -1;
        if (shell_spawner_) shell_spawner_();
    }
}

// ── endSession ────────────────────────────────────────────────────────────────

void LoginService::endSession() noexcept {
    std::lock_guard<std::mutex> lk(state_mutex_);
    // Dropping the seatd session records the utmpx logout and closes the PAM
    // session for the user. spawnGreeter() (called by the logout path) re-arms
    // greeter_active_ and shows the login screen again.
    active_session_.reset();
    std::cout << "[LoginService] Desktop session ended\n";
}

// ── isGreeterActive / greeterClient ──────────────────────────────────────────

bool LoginService::isGreeterActive() noexcept {
    return greeter_active_.load();
}

std::shared_ptr<IpcSocket> LoginService::greeterClient() noexcept {
    std::lock_guard<std::mutex> lk(state_mutex_);
    return greeter_client_.lock();
}

// ── ProcessMessage ────────────────────────────────────────────────────────────

bool LoginService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                  const shared::XBusMessage& msg) noexcept {
    using namespace shared;
    if (std::strncmp(msg.service_name_, "com.freebsdx.Login",
                     sizeof(msg.service_name_)) != 0)
        return false;

    XBusMessage resp = msg;

    switch (static_cast<LoginOpcode>(msg.opcode_)) {

    case LoginOpcode::Register: {
        std::lock_guard<std::mutex> lk(state_mutex_);
        greeter_client_ = client;
        std::cout << "[LoginService] Greeter registered\n";
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case LoginOpcode::ListUsers:
        listUsers(client);
        break;

    case LoginOpcode::Authenticate: {
        LoginAuthRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));

        char saved_user[sizeof(req.username_)];
        std::strncpy(saved_user, req.username_, sizeof(saved_user) - 1);
        saved_user[sizeof(saved_user) - 1] = '\0';

        // Brute-force throttle: reject (without touching PAM) if this account is
        // backing off or locked out from prior failures.
        auto& throttle = AuthThrottle::global();
        const auto now = std::chrono::steady_clock::now();
        auto gate = throttle.check(saved_user, now);
        bool ok = false;
        if (gate.allowed) {
            ok = pamAuthenticate(req.username_, req.password_);
            if (ok) throttle.recordSuccess(saved_user);
            else    throttle.recordFailure(saved_user, now);
            SecurityEventsService::record(ok ? "login" : "fail", saved_user, "local", ok);
        } else {
            std::cout << "[LoginService] throttled "
                      << (gate.locked_out ? "(locked out) " : "(backing off) ")
                      << saved_user << " — retry in " << gate.retry_after_s << "s\n";
            SecurityEventsService::record(gate.locked_out ? "lockout" : "throttle",
                                          saved_user, "local", false);
        }

        // Best-effort: open a real login session in xsessiond (PAM session +
        // utmpx) while we still hold the password. Kept alive for the desktop
        // session's lifetime; null if the daemon isn't running, in which case
        // login proceeds exactly as before (just without login accounting).
        if (ok) {
            active_session_ = XsessiondSession::open(saved_user, req.password_, getpid());
        }

        explicit_bzero(req.password_, sizeof(req.password_));

        LoginAuthResult result{};
        result.success_ = ok ? 1u : 0u;
        if (ok) std::strncpy(result.username_, saved_user, sizeof(result.username_) - 1);
        if (!gate.allowed) {
            result.locked_out_    = gate.locked_out ? 1u : 0u;
            result.retry_after_s_ = gate.retry_after_s;
        }

        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        resp.opcode_ = static_cast<uint32_t>(LoginOpcode::AuthResult);
        std::memcpy(resp.payload_, &result, sizeof(result));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});

        if (ok) startSession(saved_user);
        else std::cout << "[LoginService] Authentication failed for: " << saved_user << "\n";
        break;
    }

    default:
        std::cerr << "[LoginService] Unknown opcode: 0x" << std::hex << msg.opcode_ << "\n";
        break;
    }
    return true;
}

} // namespace fbx::xserver
