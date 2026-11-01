#include "xserver/speech.h"
#include "xserver/a11y_settings_service.h"
#include "xserver/config_service.h"
#include "xserver/spawn_util.h"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sys/wait.h>
#include <thread>

namespace fbx::xserver::speech {

std::string clean(const std::string& text) {
    std::string out;
    bool space = false;
    for (unsigned char c : text) {
        if (c < 0x20 || c == 0x7f) c = ' ';
        if (c == ' ') { space = !out.empty(); continue; }
        if (space) { out += ' '; space = false; }
        out += static_cast<char>(c);
        if (out.size() >= 300) break;
    }
    return out;
}

namespace {
std::mutex g_mu;
pid_t      g_voice = -1;   // the utterance in progress
}  // namespace

void say(const std::string& text, bool even_if_off) noexcept {
    if (!even_if_off && !A11ySettingsService::screenReaderOn()) return;
    const std::string line = clean(text);
    if (line.empty()) return;

    if (const char* log = std::getenv("FBX_READER_LOG"); log && *log) {
        std::lock_guard lk(g_mu);
        std::ofstream(log, std::ios::app) << line << "\n";
        return;
    }

    static const std::string engine =
        spawn::firstExecutable({"/usr/local/bin/espeak-ng", "/usr/local/bin/espeak"});
    if (engine.empty()) return;
    int rate = 175;
    try { rate = std::stoi(ConfigService::get("a11y.reader_rate", "175")); } catch (...) {}
    rate = std::clamp(rate, 80, 450);

    std::lock_guard lk(g_mu);
    if (g_voice > 0) ::kill(g_voice, SIGTERM);   // interrupt the previous utterance
    // The text is one argv element after "--": never parsed by a shell or as an option.
    g_voice = spawn::start(engine, "espeak-ng", {"-s", std::to_string(rate), "--", line});
    if (g_voice > 0) {
        const pid_t pid = g_voice;
        std::thread([pid] { ::waitpid(pid, nullptr, 0); }).detach();   // reap it
    }
}

}  // namespace fbx::xserver::speech
