// xauthcheck — setuid-root PAM authentication helper for the login greeter.
//
// The compositor (freebsdx) runs unprivileged, but PAM's pam_unix module must
// read the shadow password database (/etc/master.passwd, root-only) to verify a
// password. This tiny helper is installed setuid root and does ONLY the PAM
// check, keeping the large compositor process out of the privileged path.
//
// Protocol: read "username\npassword\n" from stdin (so the password never
// appears in argv / ps), run pam_authenticate for the "freebsdx" service, and
// exit 0 on success, 1 on failure. Nothing is printed on the happy path.
//
// Install (one time, as root):
//   install -o root -m 4755 out/xauthcheck /usr/local/libexec/xauthcheck
// (mode 4755 = setuid root, world-executable.)

#include <security/pam_appl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

extern char** environ;

namespace {

// Setuid hygiene: drop the inherited environment entirely and rebuild a minimal,
// trusted one. A setuid-root binary must never trust caller-controlled env vars
// (PATH, IFS, LD_*, PAM-affecting locale, etc.); PAM/libc may consult some of
// them internally.
void scrub_environment() {
    static char* empty[] = {nullptr};
    environ = empty;
    setenv("PATH", "/usr/bin:/bin:/usr/local/bin", 1);
}

struct PamData {
    const char* password;
};

// Feed the stored password to every PAM prompt.
int conv_fn(int num_msg, const struct pam_message** msg,
            struct pam_response** resp, void* appdata) {
    (void)msg;
    if (num_msg <= 0) return PAM_CONV_ERR;
    auto* pd = static_cast<PamData*>(appdata);
    auto* r = static_cast<struct pam_response*>(
        calloc(static_cast<size_t>(num_msg), sizeof(struct pam_response)));
    if (!r) return PAM_BUF_ERR;
    for (int i = 0; i < num_msg; ++i) {
        r[i].resp_retcode = 0;
        r[i].resp = strdup(pd->password ? pd->password : "");
    }
    *resp = r;
    return PAM_SUCCESS;
}

std::string read_line() {
    std::string s;
    int c;
    while ((c = std::getchar()) != EOF && c != '\n') s.push_back(static_cast<char>(c));
    return s;
}

}  // namespace

int main() {
    scrub_environment();

    const std::string user = read_line();
    const std::string pass = read_line();
    if (user.empty()) return 1;

    PamData pd{pass.c_str()};
    struct pam_conv conv{conv_fn, &pd};
    pam_handle_t* pamh = nullptr;
    if (pam_start("freebsdx", user.c_str(), &conv, &pamh) != PAM_SUCCESS) return 1;

    int ret = pam_authenticate(pamh, 0);
    // Even with a correct password, reject disabled/expired/locked accounts.
    if (ret == PAM_SUCCESS) ret = pam_acct_mgmt(pamh, 0);
    pam_end(pamh, ret);
    return ret == PAM_SUCCESS ? 0 : 1;
}
