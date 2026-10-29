#include "xserver/app_manager_service.h"
#include "xserver/async_shell.h"
#include "xserver/ipc_socket.h"
#include "xserver/xbus_dispatcher.h"
#include "xserver/mime_service.h"
#include "xserver/pkg_signing_service.h"
#include "xserver/shell_util.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
namespace fbx::xserver {

// ─── Catalog statics ──────────────────────────────────────────────────────────

static constexpr const char* kCatalogUrl       = "https://pkg.freebsd-x.org/catalog.json";
[[maybe_unused]] static constexpr const char* kCatalogCachePath = "/.freebsdx-catalog.json"; // relative to $HOME

struct CatalogEntry {
    std::string identifier;
    std::string name;
    std::string version;
    std::string category;
    std::string description;
    std::string download_url;
    uint32_t    size_kb{0};
};

static std::vector<CatalogEntry> g_catalog;
static std::time_t               g_catalog_ts{0};
[[maybe_unused]] static bool                      g_catalog_updating{false};

// ─── Internal JSON helpers (same minimal parser used by MimeService) ──────────

static std::string jsonValue(const std::string& json, const std::string& key) {
    std::string k = "\"" + key + "\":";
    size_t pos = json.find(k);
    if (pos == std::string::npos) return "";
    size_t s = json.find('"', pos + k.size());
    if (s == std::string::npos) return "";
    size_t e = json.find('"', s + 1);
    if (e == std::string::npos) return "";
    return json.substr(s + 1, e - s - 1);
}

static std::vector<std::string> jsonArray(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    std::string k = "\"" + key + "\"";
    size_t pos = json.find(k);
    if (pos == std::string::npos) return result;
    size_t bracket = json.find('[', pos + k.size());
    if (bracket == std::string::npos) return result;
    size_t end = json.find(']', bracket);
    if (end == std::string::npos) return result;
    std::string arr = json.substr(bracket + 1, end - bracket - 1);
    size_t i = 0;
    while (i < arr.size()) {
        size_t q1 = arr.find('"', i); if (q1 == std::string::npos) break;
        size_t q2 = arr.find('"', q1 + 1); if (q2 == std::string::npos) break;
        result.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return result;
}

// Iterates through JSON array of objects, extracting a flat list of {key->value} maps.
// Very minimal: assumes no nested objects inside each package entry.
static std::vector<std::unordered_map<std::string,std::string>>
jsonObjectArray(const std::string& json, const std::string& array_key) {
    std::vector<std::unordered_map<std::string,std::string>> result;
    std::string k = "\"" + array_key + "\"";
    size_t pos = json.find(k);
    if (pos == std::string::npos) return result;
    size_t bracket = json.find('[', pos + k.size());
    if (bracket == std::string::npos) return result;

    size_t i = bracket + 1;
    while (i < json.size()) {
        // Find next object start
        size_t obj_start = json.find('{', i);
        if (obj_start == std::string::npos) break;
        size_t obj_end = json.find('}', obj_start);
        if (obj_end == std::string::npos) break;
        // Check we haven't passed the array end
        size_t arr_end = json.find(']', bracket + 1);
        if (obj_start > arr_end) break;

        std::string obj = json.substr(obj_start + 1, obj_end - obj_start - 1);
        std::unordered_map<std::string,std::string> entry;

        // Extract all "key":"value" pairs from the object string
        size_t j = 0;
        while (j < obj.size()) {
            size_t kq1 = obj.find('"', j); if (kq1 == std::string::npos) break;
            size_t kq2 = obj.find('"', kq1+1); if (kq2 == std::string::npos) break;
            std::string key = obj.substr(kq1+1, kq2-kq1-1);
            size_t colon = obj.find(':', kq2+1); if (colon == std::string::npos) break;
            size_t vstart = colon + 1;
            while (vstart < obj.size() && obj[vstart] == ' ') ++vstart;
            std::string val;
            if (vstart < obj.size() && obj[vstart] == '"') {
                size_t vq2 = obj.find('"', vstart+1); if (vq2 == std::string::npos) break;
                val = obj.substr(vstart+1, vq2-vstart-1);
                j = vq2 + 1;
            } else {
                // Numeric value
                size_t vend = obj.find_first_of(",}", vstart);
                val = obj.substr(vstart, vend == std::string::npos ? std::string::npos : vend - vstart);
                // trim whitespace
                while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
                j = vend == std::string::npos ? obj.size() : vend + 1;
            }
            entry[key] = val;
        }
        if (!entry.empty()) result.push_back(std::move(entry));
        i = obj_end + 1;
    }
    return result;
}

static void loadCatalogFromDisk() noexcept {
    const char* home = std::getenv("HOME");
    if (!home) return;
    fs::path cache = fs::path(home) / ".freebsdx-catalog.json";
    if (!fs::exists(cache)) return;

    std::ifstream f(cache);
    if (!f) return;
    std::string json{std::istreambuf_iterator<char>(f), {}};

    g_catalog.clear();
    auto entries = jsonObjectArray(json, "packages");
    for (const auto& e : entries) {
        CatalogEntry ce;
        auto get = [&](const char* k) -> std::string {
            auto it = e.find(k); return it != e.end() ? it->second : "";
        };
        ce.identifier   = get("identifier");
        ce.name         = get("name");
        ce.version      = get("version");
        ce.category     = get("category");
        ce.description  = get("description");
        ce.download_url = get("download_url");
        std::string skb = get("size_kb");
        // Catalog is a downloaded file → treat size_kb as untrusted: stoul throws
        // on non-numeric input, which would abort the whole catalog parse.
        try { ce.size_kb = skb.empty() ? 0u : static_cast<uint32_t>(std::stoul(skb)); }
        catch (...) { ce.size_kb = 0u; }
        if (!ce.identifier.empty() && !ce.name.empty())
            g_catalog.push_back(std::move(ce));
    }

    // Extract last_updated timestamp if present
    std::string ts = jsonValue(json, "updated");
    if (!ts.empty()) g_catalog_ts = std::time(nullptr); // approximate
}

static shared::AppManagerRemotePage buildRemotePage(
        const std::vector<CatalogEntry>& entries, uint32_t page) noexcept {
    shared::AppManagerRemotePage pg{};
    pg.total_    = static_cast<uint32_t>(entries.size());
    pg.page_     = page;
    pg.has_more_ = 0;
    pg.valid_    = 0;

    if (page < entries.size()) {
        const auto& e = entries[page];
        pg.valid_    = 1;
        pg.has_more_ = (page + 1 < entries.size()) ? 1 : 0;
        std::strncpy(pg.identifier_, e.identifier.c_str(), sizeof(pg.identifier_)-1);
        std::strncpy(pg.name_,       e.name.c_str(),       sizeof(pg.name_)-1);
        std::strncpy(pg.version_,    e.version.c_str(),    sizeof(pg.version_)-1);
        std::strncpy(pg.category_,   e.category.c_str(),   sizeof(pg.category_)-1);
        std::strncpy(pg.description_,e.description.c_str(),sizeof(pg.description_)-1);
        pg.size_kb_  = e.size_kb;
        pg.installed_ = 0; // AppManager UI can cross-reference with local list
    }
    return pg;
}

static void BroadcastInstallProgress(const std::string& id, uint32_t pct,
                                      uint32_t status, const std::string& msg_str) noexcept {
    shared::AppInstallProgress prog{};
    std::strncpy(prog.identifier_, id.c_str(), sizeof(prog.identifier_)-1);
    prog.percent_ = pct;
    prog.status_  = status;
    std::strncpy(prog.message_, msg_str.c_str(), sizeof(prog.message_)-1);
    shared::XBusMessage bmsg{};
    std::strncpy(bmsg.service_name_, "com.freebsdx.AppManager", sizeof(bmsg.service_name_)-1);
    bmsg.opcode_ = static_cast<uint32_t>(shared::AppManagerOpcode::InstallProgress);
    std::memcpy(bmsg.payload_, &prog, sizeof(prog));
    BroadcastXBusMessage(bmsg);
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

struct BundleMeta {
    std::string name;
    std::string version;
    std::string identifier;
    std::string executable;
    std::string icon;
    std::vector<std::string> mime_types;
    bool valid{false};
};

static BundleMeta readManifest(const fs::path& bundle_dir) noexcept {
    BundleMeta m;
    auto path = bundle_dir / "manifest.json";
    std::ifstream f(path);
    if (!f) return m;
    std::string content((std::istreambuf_iterator<char>(f)), {});
    m.name       = jsonValue(content, "name");
    m.version    = jsonValue(content, "version");
    m.identifier = jsonValue(content, "identifier");
    m.executable = jsonValue(content, "executable");
    m.icon       = jsonValue(content, "icon");
    m.mime_types = jsonArray(content, "mime_types");
    m.valid      = !m.name.empty() && !m.identifier.empty();
    return m;
}

static shared::AppInfo makeAppInfo(const fs::path& bundle_dir,
                                   const BundleMeta& m) noexcept {
    shared::AppInfo info{};
    std::strncpy(info.identifier_, m.identifier.c_str(), sizeof(info.identifier_) - 1);
    std::strncpy(info.name_,       m.name.c_str(),       sizeof(info.name_)       - 1);
    std::strncpy(info.version_,    m.version.c_str(),    sizeof(info.version_)    - 1);

    // Resolve icon path: prefer SVG, fall back to manifest value
    fs::path svg = bundle_dir / "Resources/icon.svg";
    fs::path png = bundle_dir / (m.icon.empty() ? "Resources/icon.png" : m.icon);
    std::string icon_path = fs::exists(svg) ? svg.string() : png.string();
    std::strncpy(info.icon_path_, icon_path.c_str(), sizeof(info.icon_path_) - 1);
    info.installed_ = 1;
    return info;
}

// Run a shell command, return true on success.
static bool runCmd(const std::string& cmd) noexcept {
    std::cout << "[AppManager] " << cmd << "\n";
    // Cap so a wedged extract/install command can't hang the compositor forever.
    int ret = fbx::xserver::spawnStatus(("timeout -k 5 300 " + cmd).c_str());
    return ret == 0;
}

// ─── AppManagerService ────────────────────────────────────────────────────────

void AppManagerService::setAppsDir(const std::string& dir) noexcept {
    apps_dir_ = dir;
}

const std::string& AppManagerService::appsDir() noexcept {
    return apps_dir_;
}

void AppManagerService::broadcastProgress(const std::string& identifier,
                                          uint32_t percent,
                                          uint32_t status,
                                          const std::string& message) noexcept {
    using namespace shared;
    XBusMessage msg{};
    std::strncpy(msg.service_name_, "com.freebsdx.AppManager",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(AppManagerOpcode::InstallProgress);

    AppInstallProgress prog{};
    std::strncpy(prog.identifier_, identifier.c_str(), sizeof(prog.identifier_) - 1);
    prog.percent_ = percent;
    prog.status_  = status;
    std::strncpy(prog.message_, message.c_str(), sizeof(prog.message_) - 1);
    std::memcpy(msg.payload_, &prog, sizeof(prog));

    BroadcastXBusMessage(msg);
    std::cout << "[AppManager] " << identifier << " " << percent << "% — " << message << "\n";
}

void AppManagerService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                       const shared::XBusMessage& msg) noexcept {
    using namespace shared;
    XBusMessage resp = msg;

    switch (static_cast<AppManagerOpcode>(msg.opcode_)) {

    // ── ListApps ────────────────────────────────────────────────────────────
    case AppManagerOpcode::ListApps: {
        // Requested page index is in first 4 bytes of payload (0 = start)
        uint32_t requested_page = 0;
        std::memcpy(&requested_page, msg.payload_, sizeof(requested_page));

        // Enumerate installed bundles
        std::vector<fs::path> bundles;
        try {
            if (fs::exists(apps_dir_)) {
                for (const auto& e : fs::directory_iterator(apps_dir_)) {
                    if (e.is_directory() && e.path().extension() == ".xapp")
                        bundles.push_back(e.path());
                }
            }
        } catch (...) {}

        uint32_t total = static_cast<uint32_t>(bundles.size());

        if (total == 0 || requested_page >= total) {
            AppList empty{};
            empty.total_ = total;
            std::memset(resp.payload_, 0, sizeof(resp.payload_));
            std::memcpy(resp.payload_, &empty, sizeof(empty));
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
            break;
        }

        // Send one entry per response; client calls ListApps with next page index
        const auto& bundle = bundles[requested_page];
        auto meta = readManifest(bundle);
        if (!meta.valid) {
            // Skip invalid bundles by advancing to next
            AppList skip{};
            skip.count_    = 0;
            skip.total_    = total;
            skip.page_     = requested_page;
            skip.has_more_ = (requested_page + 1 < total) ? 1 : 0;
            std::memset(resp.payload_, 0, sizeof(resp.payload_));
            std::memcpy(resp.payload_, &skip, sizeof(skip));
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
            break;
        }

        AppList list{};
        list.count_    = 1;
        list.total_    = total;
        list.page_     = requested_page;
        list.has_more_ = (requested_page + 1 < total) ? 1 : 0;
        list.app_      = makeAppInfo(bundle, meta);
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &list, sizeof(list));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    // ── Install ─────────────────────────────────────────────────────────────
    case AppManagerOpcode::Install: {
        AppInstallRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        std::string src_path(req.path_, strnlen(req.path_, sizeof(req.path_)));

        // Acknowledge immediately so the client doesn't time out
        uint32_t ack = 1;
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &ack, sizeof(ack));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});

        std::string identifier = "unknown";

        broadcastProgress(identifier, 5, 0, "Validating package…");

        // ── Case 1: .xpkg archive ──────────────────────────────────────────
        if (src_path.size() > 5 &&
            src_path.substr(src_path.size() - 5) == ".xpkg") {

            if (!fs::exists(src_path)) {
                broadcastProgress(identifier, 0, 2,
                    "File not found: " + src_path);
                break;
            }

            // ── Signature verification ─────────────────────────────────────
            broadcastProgress(identifier, 8, 0, "Checking signature…");
            {
                auto policy = PkgSigningService::GetPolicy();
                auto vr     = PkgSigningService::VerifyPackage(src_path);
                if (vr.ok) {
                    broadcastProgress(identifier, 12, 0,
                        "Signature OK — " + vr.message);
                } else if (policy.require_signed_) {
                    broadcastProgress(identifier, 0, 2,
                        "Signature verification failed: " + vr.message);
                    break;
                } else {
                    // warn only
                    std::cerr << "[AppManager] Warning: " << src_path
                              << " — " << vr.message << "\n";
                }
            }

            // Extract to a temp directory
            std::string tmp_dir = "/tmp/xpkg-extract-XXXXXX";
            {
                char buf[64];
                std::strncpy(buf, tmp_dir.c_str(), sizeof(buf) - 1);
                if (!mkdtemp(buf)) {
                    broadcastProgress(identifier, 0, 2, "Failed to create temp dir");
                    break;
                }
                tmp_dir = buf;
            }

            broadcastProgress(identifier, 20, 0, "Extracting archive…");
            // src_path comes from the client: quote it properly (a bare '…'
            // let a file named "x';cmd;'.xpkg" run cmd).
            std::string extract_cmd = "tar -xzf " + shellQuote(src_path) + " -C " + shellQuote(tmp_dir);
            if (!runCmd(extract_cmd)) {
                broadcastProgress(identifier, 0, 2, "Extraction failed");
                fs::remove_all(tmp_dir);
                break;
            }

            // Find the .xapp inside the temp dir
            fs::path bundle_in_tmp;
            try {
                for (const auto& e : fs::directory_iterator(tmp_dir)) {
                    if (e.is_directory() && e.path().extension() == ".xapp") {
                        bundle_in_tmp = e.path();
                        break;
                    }
                }
            } catch (...) {}

            if (bundle_in_tmp.empty()) {
                broadcastProgress(identifier, 0, 2,
                    "No .xapp bundle found inside archive");
                fs::remove_all(tmp_dir);
                break;
            }

            auto meta = readManifest(bundle_in_tmp);
            if (!meta.valid) {
                broadcastProgress(identifier, 0, 2,
                    "Invalid or missing manifest.json");
                fs::remove_all(tmp_dir);
                break;
            }
            identifier = meta.identifier;

            broadcastProgress(identifier, 50, 0, "Installing " + meta.name + "…");

            fs::path dest = fs::path(apps_dir_) / bundle_in_tmp.filename();
            try {
                if (fs::exists(dest)) fs::remove_all(dest);
                fs::copy(bundle_in_tmp, dest, fs::copy_options::recursive);
            } catch (const std::exception& ex) {
                broadcastProgress(identifier, 0, 2,
                    std::string("Copy failed: ") + ex.what());
                fs::remove_all(tmp_dir);
                break;
            }
            fs::remove_all(tmp_dir);

            // Register MIME types
            broadcastProgress(identifier, 85, 0, "Registering file associations…");
            MimeService::loadFromAppsDir(apps_dir_);

            broadcastProgress(identifier, 100, 1,
                meta.name + " installed successfully.");

        // ── Case 2: .xapp directory ────────────────────────────────────────
        } else if (fs::is_directory(src_path) &&
                   fs::path(src_path).extension() == ".xapp") {

            auto meta = readManifest(src_path);
            if (!meta.valid) {
                broadcastProgress(identifier, 0, 2,
                    "Invalid or missing manifest.json in " + src_path);
                break;
            }
            identifier = meta.identifier;
            broadcastProgress(identifier, 30, 0, "Installing " + meta.name + "…");

            fs::path dest = fs::path(apps_dir_) / fs::path(src_path).filename();
            try {
                if (fs::exists(dest)) fs::remove_all(dest);
                fs::copy(src_path, dest, fs::copy_options::recursive);
            } catch (const std::exception& ex) {
                broadcastProgress(identifier, 0, 2,
                    std::string("Copy failed: ") + ex.what());
                break;
            }

            broadcastProgress(identifier, 85, 0, "Registering file associations…");
            MimeService::loadFromAppsDir(apps_dir_);
            broadcastProgress(identifier, 100, 1,
                meta.name + " installed successfully.");

        } else {
            broadcastProgress(identifier, 0, 2,
                "Unsupported source: must be a .xpkg file or .xapp directory");
        }
        break;
    }

    // ── Uninstall ────────────────────────────────────────────────────────────
    case AppManagerOpcode::Uninstall: {
        AppIdentifierRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        std::string target_id(req.identifier_, strnlen(req.identifier_, sizeof(req.identifier_)));

        // Acknowledge
        uint32_t ack = 1;
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &ack, sizeof(ack));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});

        broadcastProgress(target_id, 10, 0, "Locating " + target_id + "…");

        bool found = false;
        try {
            if (fs::exists(apps_dir_)) {
                for (const auto& e : fs::directory_iterator(apps_dir_)) {
                    if (!e.is_directory() || e.path().extension() != ".xapp") continue;
                    auto meta = readManifest(e.path());
                    if (!meta.valid || meta.identifier != target_id) continue;

                    broadcastProgress(target_id, 50, 0, "Removing " + meta.name + "…");
                    fs::remove_all(e.path());
                    broadcastProgress(target_id, 100, 1,
                        meta.name + " uninstalled.");
                    found = true;
                    break;
                }
            }
        } catch (const std::exception& ex) {
            broadcastProgress(target_id, 0, 2,
                std::string("Removal failed: ") + ex.what());
            break;
        }

        if (!found)
            broadcastProgress(target_id, 0, 2,
                "App not found: " + target_id);
        break;
    }

    // ── GetAppInfo ───────────────────────────────────────────────────────────
    case AppManagerOpcode::GetAppInfo: {
        AppIdentifierRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        std::string target_id(req.identifier_, strnlen(req.identifier_, sizeof(req.identifier_)));

        AppList result{};
        try {
            if (fs::exists(apps_dir_)) {
                for (const auto& e : fs::directory_iterator(apps_dir_)) {
                    if (!e.is_directory() || e.path().extension() != ".xapp") continue;
                    auto meta = readManifest(e.path());
                    if (!meta.valid || meta.identifier != target_id) continue;
                    result.count_    = 1;
                    result.total_    = 1;
                    result.has_more_ = 0;
                    result.app_      = makeAppInfo(e.path(), meta);
                    break;
                }
            }
        } catch (...) {}

        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &result, sizeof(result));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    default:
        std::cerr << "[AppManager] Unknown opcode 0x" << std::hex << msg.opcode_
                  << std::dec << "\n";
        break;
    }
}

// ─── Remote catalog methods ───────────────────────────────────────────────────

shared::AppManagerCatalogState AppManagerService::updateCatalog() noexcept {
    shared::AppManagerCatalogState st{};
    std::strncpy(st.catalog_url_, kCatalogUrl, sizeof(st.catalog_url_)-1);

    const char* home = std::getenv("HOME");
    if (!home) { st.error_ = 1; std::strncpy(st.error_msg_, "HOME not set", 12); return st; }

    fs::path cache = fs::path(home) / ".freebsdx-catalog.json";

    // Cap the network fetch: no repo/DNS must never hang the compositor.
    std::string cmd = "timeout -k 2 20 fetch -q -o \"" + cache.string() + "\" " + kCatalogUrl + " 2>/dev/null"
                    + " || timeout -k 2 20 curl -s -o \"" + cache.string() + "\" " + kCatalogUrl + " 2>/dev/null";
    int ret = fbx::xserver::spawnStatus(cmd.c_str());

    if (ret == 0 && fs::exists(cache)) {
        loadCatalogFromDisk();
        st.total_packages_ = static_cast<uint32_t>(g_catalog.size());
        st.last_updated_   = static_cast<uint32_t>(std::time(nullptr));
        g_catalog_ts       = std::time(nullptr);
    } else {
        st.error_ = 1;
        std::strncpy(st.error_msg_, "Catalog download failed", 23);
    }
    return st;
}

shared::AppManagerRemotePage AppManagerService::listRemote(uint32_t page) noexcept {
    if (g_catalog.empty()) loadCatalogFromDisk();
    return buildRemotePage(g_catalog, page);
}

shared::AppManagerRemotePage AppManagerService::searchRemote(
        const shared::AppManagerSearchRequest& req) noexcept {
    if (g_catalog.empty()) loadCatalogFromDisk();

    std::string ql = req.query_;
    std::transform(ql.begin(), ql.end(), ql.begin(), ::tolower);

    std::vector<CatalogEntry> matches;
    for (const auto& e : g_catalog) {
        std::string nl = e.name; std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
        std::string dl = e.description; std::transform(dl.begin(), dl.end(), dl.begin(), ::tolower);
        std::string il = e.identifier; std::transform(il.begin(), il.end(), il.begin(), ::tolower);
        if (nl.find(ql) != std::string::npos ||
            dl.find(ql) != std::string::npos ||
            il.find(ql) != std::string::npos)
            matches.push_back(e);
    }
    return buildRemotePage(matches, req.page_);
}

void AppManagerService::installRemote(
        const shared::AppManagerRemoteInstallRequest& req,
        std::shared_ptr<IpcSocket> requester) noexcept {
    std::string url  = req.download_url_;
    std::string id   = req.identifier_;

    // The identifier is interpolated into a filesystem path below — reject
    // anything but a safe app-id charset so it can't traverse the path or
    // smuggle shell metacharacters.
    bool id_ok = !id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '_' || c == '-';
        });
    if (!id_ok) {
        BroadcastInstallProgress(id, 0, 2, "Invalid app identifier");
        return;
    }

    // If download_url not provided, look it up from in-memory catalog
    if (url.empty()) {
        for (const auto& e : g_catalog) {
            if (e.identifier == id) { url = e.download_url; break; }
        }
    }

    // shellQuote the URL and temp path: both derive from client input and go
    // into a shell command. (Double-quoting alone doesn't stop $(), ``, or ".)
    std::string tmp = "/tmp/freebsdx-remote-" + id + ".xpkg";
    std::string cmd = "fetch -q -o " + shellQuote(tmp) + " " + shellQuote(url) + " 2>/dev/null"
                    + " || curl -s -o " + shellQuote(tmp) + " " + shellQuote(url) + " 2>/dev/null";

    std::thread([cmd, tmp, id, requester]() {
        try {
            BroadcastInstallProgress(id, 10, 0, "Downloading...");
            int ret = fbx::xserver::spawnStatus(cmd.c_str());
            if (ret != 0 || !fs::exists(tmp)) {
                BroadcastInstallProgress(id, 0, 2, "Download failed");
                return;
            }
            BroadcastInstallProgress(id, 50, 0, "Installing...");
            shared::AppInstallRequest ireq{};
            std::strncpy(ireq.path_, tmp.c_str(), sizeof(ireq.path_)-1);
            shared::XBusMessage fake_msg{};
            fake_msg.magic_ = shared::XBUS_MAGIC;
            std::strncpy(fake_msg.service_name_, "com.freebsdx.AppManager",
                         sizeof(fake_msg.service_name_)-1);
            fake_msg.opcode_ = static_cast<uint32_t>(shared::AppManagerOpcode::Install);
            std::memcpy(fake_msg.payload_, &ireq, sizeof(ireq));
            AppManagerService::ProcessMessage(requester, fake_msg);
            fs::remove(tmp);
        } catch (...) {
            BroadcastInstallProgress(id, 0, 2, "Internal error");
        }
    }).detach();
}

} // namespace fbx::xserver
