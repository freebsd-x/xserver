#include "xserver/login_service.h"
#include "xserver/spawn_util.h"
#include "xserver/auth_helper.h"
#include "xserver/auth_throttle.h"
#include "xserver/security_events_service.h"
#include "xserver/xsessiond_session.h"
#include "xserver/ipc_socket.h"
#include <security/pam_appl.h>
#include <sys/types.h>
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

// Shared full-chain password check (declared in auth_helper.h). Both the login
// greeter and the screen locker route through this so they enforce identical
// policy — notably pam_acct_mgmt on the in-process fallback, which login's old
// inline path skipped (a disabled/expired account could have logged in).
bool pamAuthenticateFull(const char* service, const char* user,
                         const char* password) noexcept {
    // The xsessiond daemon does PAM as root, keeping the compositor
    // unprivileged. Without it, fall back to in-process PAM (below), which
    // only works when the compositor itself runs as root.
    if (int s = authViaXsessiond(user, password); s >= 0) return s == 1;
    if (::geteuid() != 0)
        std::cerr << "[auth] xsessiond unreachable and not root: PAM can't read the "
                     "password database (start xsessiond)\n";

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
    // posix_spawn, not fork + setenv (unsafe after fork here; a hang left the
    // login screen missing — spawn_util.h). ./xlogin is the dev build (builddir/).
    greeter_pid_ = spawn::start(
        spawn::firstExecutable({"./xlogin", "./out/xlogin", "/usr/local/bin/xlogin"}), "xlogin", {},
        {.env = {{"LD_LIBRARY_PATH", "./out"}, {"XBUS_SOCKET", "/tmp/freebsdx.sock"}}});
    if (greeter_pid_ < 0) {
        // No greeter binary: straight to the shell (dev workflow without xlogin).
        std::cerr << "[LoginService] no xlogin greeter found — starting the shell directly\n";
        greeter_active_ = false;
        if (shell_spawner_) shell_spawner_();
        return;
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
