// font_manager_service.cc — com.freebsdx.FontManager XBus service
//
// Enumerates installed fonts via fc-list, installs fonts by copying to
// ~/.fonts + running fc-cache, and removes user-installed fonts.
// All font data is cached after the first enumeration; install/remove
// invalidate the cache and broadcast FontsChanged to all clients.

#include "xserver/font_manager_service.h"
#include "xserver/xbus_dispatcher.h"
#include "xserver/async_shell.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <unistd.h>
#include <sys/stat.h>

namespace fbx {

namespace fs = std::filesystem;

std::mutex                    FontManagerService::mutex_;
std::vector<shared::FontInfo> FontManagerService::cache_;
bool                          FontManagerService::cache_ready_ = false;

// ── isValidFontPath ───────────────────────────────────────────────────────────
// Allows only alphanumeric + /._- characters, must start with '/', max 255.

bool FontManagerService::isValidFontPath(const std::string& path) noexcept {
    if (path.empty() || path[0] != '/' || path.size() > 255) return false;
    for (unsigned char c : path) {
        if (!std::isalnum(c) && c != '/' && c != '.' && c != '_' && c != '-' && c != ' ')
            return false;
    }
    return true;
}

// ── isUserFont ────────────────────────────────────────────────────────────────

bool FontManagerService::isUserFont(const std::string& path) noexcept {
    const char* home = std::getenv("HOME");
    if (!home) return false;
    std::string prefix = std::string(home) + "/.fonts/";
    return path.rfind(prefix, 0) == 0;
}

// ── detectFormat ─────────────────────────────────────────────────────────────

std::string FontManagerService::detectFormat(const std::string& path) noexcept {
    auto ext_pos = path.rfind('.');
    if (ext_pos == std::string::npos) return "?";
    std::string ext = path.substr(ext_pos + 1);
    // lower-case
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "ttf")  return "TTF";
    if (ext == "otf")  return "OTF";
    if (ext == "ttc")  return "TTC";
    if (ext == "pfa" || ext == "pfb") return "Type1";
    if (ext == "bdf")  return "BDF";
    if (ext == "pcf" || ext == "pcf.gz") return "PCF";
    if (ext == "woff") return "WOFF";
    if (ext == "woff2") return "WOFF2";
    return "?";
}

// ── enumerateFonts ────────────────────────────────────────────────────────────
// Runs fc-list and parses "family|style|file" lines.

std::vector<shared::FontInfo> FontManagerService::enumerateFonts() noexcept {
    std::vector<shared::FontInfo> result;

    // fc-list --format is available on fontconfig ≥ 2.9 (FreeBSD ports default).
    // Bounded so a stuck fontconfig scan can't hang the event loop.
    const char* home = std::getenv("HOME");
    std::string user_prefix = home ? (std::string(home) + "/.fonts/") : "";

    std::istringstream fs_in(xserver::runCapped(
        "fc-list --format '%{family[0]}|%{style[0]}|%{file}\n' 2>/dev/null | sort -u"));
    std::string line;
    while (std::getline(fs_in, line)) {
        // strip trailing newline
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (line.empty()) continue;

        // Split on '|'
        auto p1 = line.find('|');
        if (p1 == std::string::npos) continue;
        auto p2 = line.find('|', p1 + 1);
        if (p2 == std::string::npos) continue;

        std::string family = line.substr(0, p1);
        std::string style  = line.substr(p1 + 1, p2 - p1 - 1);
        std::string path   = line.substr(p2 + 1);

        // family may be "DejaVu Sans,DejaVu Sans Condensed" — take part before first comma
        auto comma = family.find(',');
        if (comma != std::string::npos) family = family.substr(0, comma);
        // same for style
        comma = style.find(',');
        if (comma != std::string::npos) style = style.substr(0, comma);

        // Trim leading/trailing spaces
        auto trim = [](std::string& s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
        };
        trim(family); trim(style); trim(path);

        if (family.empty() || path.empty()) continue;

        shared::FontInfo fi{};
        std::strncpy(fi.family_, family.c_str(), sizeof(fi.family_) - 1);
        std::strncpy(fi.style_,  style.c_str(),  sizeof(fi.style_)  - 1);
        std::strncpy(fi.path_,   path.c_str(),   sizeof(fi.path_)   - 1);
        std::string fmt = detectFormat(path);
        std::strncpy(fi.format_, fmt.c_str(),    sizeof(fi.format_) - 1);
        fi.is_user_installed_ = (!user_prefix.empty() && path.rfind(user_prefix, 0) == 0) ? 1u : 0u;

        result.push_back(fi);
    }
    return result;
}

// ── installFont ───────────────────────────────────────────────────────────────

bool FontManagerService::installFont(const std::string& src_path) noexcept {
    if (!isValidFontPath(src_path)) return false;

    const char* home = std::getenv("HOME");
    if (!home) return false;

    // Ensure ~/.fonts exists
    std::string fonts_dir = std::string(home) + "/.fonts";
    std::error_code ec;
    fs::create_directories(fonts_dir, ec);

    // Extract filename
    std::string fname = src_path.substr(src_path.rfind('/') + 1);
    if (fname.empty()) return false;

    std::string dest = fonts_dir + "/" + fname;

    // Validate dest path
    if (!isValidFontPath(dest)) return false;

    // Copy the font file
    fs::copy_file(src_path, dest,
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[FontManager] copy failed: " << ec.message() << "\n";
        return false;
    }

    // Rebuild font cache for user fonts (non-blocking, quick)
    std::string cmd = "fc-cache -f " + fonts_dir + " 2>/dev/null";
    (void)fbx::xserver::spawnStatus(cmd.c_str());

    return true;
}

// ── removeFont ────────────────────────────────────────────────────────────────

bool FontManagerService::removeFont(const std::string& path) noexcept {
    if (!isValidFontPath(path)) return false;
    if (!isUserFont(path)) {
        std::cerr << "[FontManager] removeFont denied: not a user font: " << path << "\n";
        return false;
    }

    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        std::cerr << "[FontManager] remove failed: " << ec.message() << "\n";
        return false;
    }

    const char* home = std::getenv("HOME");
    if (home) {
        std::string cmd = "fc-cache -f " + std::string(home) + "/.fonts 2>/dev/null";
        (void)fbx::xserver::spawnStatus(cmd.c_str());
    }

    return true;
}

// ── sendStatus ────────────────────────────────────────────────────────────────

static void sendStatus(std::shared_ptr<fbx::IpcSocket> client,
                       const shared::XBusMessage& req,
                       bool ok, const std::string& msg) noexcept {
    shared::XBusMessage resp = req;
    shared::FontStatusResponse st{};
    st.success_ = ok ? 1u : 0u;
    std::strncpy(st.message_, msg.c_str(), sizeof(st.message_) - 1);
    std::memcpy(resp.payload_, &st, sizeof(st));
    (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
}

// ── broadcastFontsChanged ─────────────────────────────────────────────────────

static void broadcastFontsChanged() noexcept {
    shared::XBusMessage msg{};
    msg.magic_ = shared::XBUS_MAGIC;
    std::strncpy(msg.service_name_, "com.freebsdx.FontManager",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::FontManagerOpcode::FontsChanged);
    xserver::BroadcastXBusMessage(msg);
}

// ── init ──────────────────────────────────────────────────────────────────────

void FontManagerService::init() noexcept {
    // Pre-warm the font cache on a background thread so the first ListFonts
    // request is served from memory.
    std::thread([]() {
        try {
            auto fonts = enumerateFonts();
            std::lock_guard<std::mutex> lk(mutex_);
            cache_       = std::move(fonts);
            cache_ready_ = true;
            std::cout << "[FontManager] cached " << cache_.size() << " fonts\n";
        } catch (...) {
            std::lock_guard<std::mutex> lk(mutex_);
            cache_ready_ = true; // mark ready so callers don't spin-wait
            std::cerr << "[FontManager] cache warm-up failed\n";
        }
    }).detach();
}

// ── handleMessage ─────────────────────────────────────────────────────────────

void FontManagerService::handleMessage(const shared::XBusMessage& msg,
                                       std::shared_ptr<IpcSocket> client) noexcept {
    const auto op = static_cast<shared::FontManagerOpcode>(msg.opcode_);

    switch (op) {

    case shared::FontManagerOpcode::ListFonts: {
        shared::FontPageRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t page = req.page_;

        std::vector<shared::FontInfo> snapshot;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!cache_ready_) {
                // Cache not ready — re-enumerate synchronously
                cache_      = enumerateFonts();
                cache_ready_ = true;
            }
            snapshot = cache_;
        }

        if (page >= snapshot.size()) {
            // Past end — send empty terminal page
            shared::FontListPage pg{};
            pg.page_     = page;
            pg.has_more_ = 0;
            pg.total_    = static_cast<uint32_t>(snapshot.size());
            pg.count_    = 0;
            shared::XBusMessage resp = msg;
            std::memcpy(resp.payload_, &pg, sizeof(pg));
            (void)client->sendMessage(
                {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
            break;
        }

        shared::FontListPage pg{};
        pg.page_     = page;
        pg.has_more_ = (page + 1 < snapshot.size()) ? 1u : 0u;
        pg.total_    = static_cast<uint32_t>(snapshot.size());
        pg.count_    = 1;
        pg.font_     = snapshot[page];

        shared::XBusMessage resp = msg;
        std::memcpy(resp.payload_, &pg, sizeof(pg));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case shared::FontManagerOpcode::InstallFont: {
        shared::FontInstallRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.path_[sizeof(req.path_) - 1] = '\0';
        std::string path = req.path_;

        // Run async so we can ack immediately
        std::thread([path, msg, client]() {
            try {
                bool ok = installFont(path);
                if (ok) {
                    auto fresh = enumerateFonts();
                    {
                        std::lock_guard<std::mutex> lk(mutex_);
                        cache_ = std::move(fresh);
                    }
                    broadcastFontsChanged();
                }
                sendStatus(client, msg, ok,
                           ok ? "Font installed." : "Install failed.");
            } catch (...) {
                sendStatus(client, msg, false, "Internal error");
            }
        }).detach();
        break;
    }

    case shared::FontManagerOpcode::RemoveFont: {
        shared::FontRemoveRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.path_[sizeof(req.path_) - 1] = '\0';
        std::string path = req.path_;

        std::thread([path, msg, client]() {
            try {
                bool ok = removeFont(path);
                if (ok) {
                    auto fresh = enumerateFonts();
                    {
                        std::lock_guard<std::mutex> lk(mutex_);
                        cache_ = std::move(fresh);
                    }
                    broadcastFontsChanged();
                }
                sendStatus(client, msg, ok,
                           ok ? "Font removed." : "Remove failed (system fonts cannot be removed).");
            } catch (...) {
                sendStatus(client, msg, false, "Internal error");
            }
        }).detach();
        break;
    }

    default:
        sendStatus(client, msg, false, "Unknown opcode");
        break;
    }
}

} // namespace fbx
