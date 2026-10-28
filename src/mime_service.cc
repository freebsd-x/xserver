#include "xserver/mime_service.h"
#include "xserver/ipc_socket.h"
#include "xserver/config_service.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstring>

namespace fbx::xserver {

// ─── Extension → MIME type table ─────────────────────────────────────────────

static constexpr std::pair<std::string_view, std::string_view> kExtMime[] = {
    {".txt",   "text/plain"},
    {".md",    "text/markdown"},
    {".markdown","text/markdown"},
    {".html",  "text/html"},
    {".htm",   "text/html"},
    {".css",   "text/css"},
    {".js",    "text/javascript"},
    {".mjs",   "text/javascript"},
    {".json",  "application/json"},
    {".xml",   "application/xml"},
    {".svg",   "image/svg+xml"},
    {".png",   "image/png"},
    {".jpg",   "image/jpeg"},
    {".jpeg",  "image/jpeg"},
    {".gif",   "image/gif"},
    {".webp",  "image/webp"},
    {".ico",   "image/x-icon"},
    {".pdf",   "application/pdf"},
    {".sh",    "application/x-sh"},
    {".bash",  "application/x-sh"},
    {".zsh",   "application/x-sh"},
    {".cc",    "text/x-c++"},
    {".cpp",   "text/x-c++"},
    {".cxx",   "text/x-c++"},
    {".hpp",   "text/x-c++"},
    {".hh",    "text/x-c++"},
    {".c",     "text/x-c"},
    {".h",     "text/x-c"},
    {".py",    "text/x-python"},
    {".rs",    "text/x-rust"},
    {".go",    "text/x-go"},
    {".java",  "text/x-java-source"},
    {".rb",    "text/x-ruby"},
    {".ts",    "text/typescript"},
    {".toml",  "application/toml"},
    {".yaml",  "application/yaml"},
    {".yml",   "application/yaml"},
    {".zip",   "application/zip"},
    {".tar",   "application/x-tar"},
    {".gz",    "application/gzip"},
    {".bz2",   "application/x-bzip2"},
    {".xz",    "application/x-xz"},
    {".mp3",   "audio/mpeg"},
    {".ogg",   "audio/ogg"},
    {".wav",   "audio/wav"},
    {".flac",  "audio/flac"},
    {".mp4",   "video/mp4"},
    {".mkv",   "video/x-matroska"},
    {".webm",  "video/webm"},
};

// ─── Internal JSON helpers ────────────────────────────────────────────────────

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
        size_t q1 = arr.find('"', i);
        if (q1 == std::string::npos) break;
        size_t q2 = arr.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        result.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return result;
}

// ─── MimeService::loadFromAppsDir ────────────────────────────────────────────

void MimeService::loadFromAppsDir(const std::string& apps_dir) noexcept {
    try {
        if (!std::filesystem::exists(apps_dir)) return;
        for (const auto& entry : std::filesystem::directory_iterator(apps_dir)) {
            if (!entry.is_directory() || entry.path().extension() != ".xapp") continue;
            auto manifest = entry.path() / "manifest.json";
            if (!std::filesystem::exists(manifest)) continue;

            std::ifstream f(manifest);
            std::string content((std::istreambuf_iterator<char>(f)), {});

            std::string id   = jsonValue(content, "identifier");
            std::string name = jsonValue(content, "name");
            auto types       = jsonArray(content, "mime_types");

            if (id.empty() || types.empty()) continue;

            app_names_[id] = name;
            for (const auto& mime : types) {
                // Apply any user override stored via SetDefaultHandler
                auto& handlers = registry_[mime];
                // Avoid duplicates
                bool exists = std::any_of(handlers.begin(), handlers.end(),
                    [&id](const AppEntry& e){ return e.app_id == id; });
                if (!exists)
                    handlers.push_back({id, name});
            }
            std::cout << "[MimeService] Registered " << name
                      << " (" << id << ") for " << types.size() << " type(s)\n";
        }
    } catch (const std::exception& ex) {
        std::cerr << "[MimeService] loadFromAppsDir error: " << ex.what() << "\n";
    }

    // Apply persisted user overrides: config key "mime.default.<mime>" = app_id
    // Promote the stored app to the front of each affected handler list.
    for (auto& [mime, handlers] : registry_) {
        std::string override_key = "mime.default." + mime;
        std::string stored = ConfigService::get(override_key);
        if (stored.empty()) continue;
        auto it = std::find_if(handlers.begin(), handlers.end(),
            [&stored](const AppEntry& e){ return e.app_id == stored; });
        if (it != handlers.end() && it != handlers.begin())
            std::rotate(handlers.begin(), it, it + 1); // move to front
    }
}

// ─── Public queries ───────────────────────────────────────────────────────────

std::string MimeService::mimeFromExtension(std::string_view ext) noexcept {
    // Normalise to lower-case for comparison
    std::string low(ext);
    std::transform(low.begin(), low.end(), low.begin(), ::tolower);
    for (const auto& [e, m] : kExtMime) {
        if (e == low) return std::string(m);
    }
    return "application/octet-stream";
}

std::string MimeService::defaultHandler(const std::string& mime_type) noexcept {
    auto it = registry_.find(mime_type);
    if (it == registry_.end() || it->second.empty()) return "";
    return it->second.front().app_id;
}

std::vector<std::string> MimeService::allHandlers(const std::string& mime_type) noexcept {
    std::vector<std::string> ids;
    auto it = registry_.find(mime_type);
    if (it == registry_.end()) return ids;
    for (const auto& e : it->second) ids.push_back(e.app_id);
    return ids;
}

// ─── XBus message handler ─────────────────────────────────────────────────────

void MimeService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                  const shared::XBusMessage& msg) noexcept {
    using namespace shared;
    XBusMessage resp = msg;

    switch (static_cast<MimeOpcode>(msg.opcode_)) {

    case MimeOpcode::QueryHandler: {
        MimeQueryRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));

        std::string mime;
        if (req.is_path_) {
            // Derive MIME from the file extension
            std::filesystem::path p(req.path_);
            mime = mimeFromExtension(p.extension().string());
        } else {
            mime = std::string(req.path_, strnlen(req.path_, sizeof(req.path_)));
        }

        MimeHandlerList list{};
        list.page_     = 0;
        list.has_more_ = 0;

        auto it = registry_.find(mime);
        if (it != registry_.end() && !it->second.empty()) {
            const AppEntry& best = it->second.front();
            list.count_ = 1;
            list.total_ = static_cast<uint32_t>(it->second.size());
            std::strncpy(list.handler_.app_id_,   best.app_id.c_str(),  sizeof(list.handler_.app_id_)   - 1);
            std::strncpy(list.handler_.app_name_,  best.app_name.c_str(), sizeof(list.handler_.app_name_) - 1);
            std::strncpy(list.handler_.mime_type_, mime.c_str(),          sizeof(list.handler_.mime_type_) - 1);
            list.handler_.is_default_ = 1;
        }

        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &list, sizeof(list));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});

        if (list.count_ == 0)
            std::cout << "[MimeService] QueryHandler: no handler for " << mime << "\n";
        else
            std::cout << "[MimeService] QueryHandler: " << mime
                      << " → " << list.handler_.app_id_ << "\n";
        break;
    }

    case MimeOpcode::SetDefaultHandler: {
        MimeSetDefaultRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        std::string mime(req.mime_type_, strnlen(req.mime_type_, sizeof(req.mime_type_)));
        std::string app_id(req.app_id_, strnlen(req.app_id_, sizeof(req.app_id_)));

        setDefault(mime, app_id);
        break;
    }

    case MimeOpcode::ListHandlers: {
        MimeQueryRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));

        std::string mime;
        if (req.is_path_) {
            std::filesystem::path p(req.path_);
            mime = mimeFromExtension(p.extension().string());
        } else {
            mime = std::string(req.path_, strnlen(req.path_, sizeof(req.path_)));
        }

        // Return handlers one page at a time (one per message)
        // For simplicity, send all handlers sequentially — clients reassemble
        auto it = registry_.find(mime);
        if (it == registry_.end() || it->second.empty()) {
            MimeHandlerList empty{};
            std::memset(resp.payload_, 0, sizeof(resp.payload_));
            std::memcpy(resp.payload_, &empty, sizeof(empty));
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
            break;
        }

        const auto& handlers = it->second;
        uint32_t total = static_cast<uint32_t>(handlers.size());
        for (uint32_t i = 0; i < total; ++i) {
            MimeHandlerList page{};
            page.count_    = 1;
            page.total_    = total;
            page.page_     = i;
            page.has_more_ = (i + 1 < total) ? 1 : 0;
            std::strncpy(page.handler_.app_id_,    handlers[i].app_id.c_str(),   sizeof(page.handler_.app_id_)   - 1);
            std::strncpy(page.handler_.app_name_,   handlers[i].app_name.c_str(),  sizeof(page.handler_.app_name_) - 1);
            std::strncpy(page.handler_.mime_type_,  mime.c_str(),                  sizeof(page.handler_.mime_type_) - 1);
            page.handler_.is_default_ = (i == 0) ? 1 : 0;
            std::memset(resp.payload_, 0, sizeof(resp.payload_));
            std::memcpy(resp.payload_, &page, sizeof(page));
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        }
        break;
    }

    default:
        std::cerr << "[MimeService] Unknown opcode 0x" << std::hex << msg.opcode_
                  << std::dec << "\n";
        break;
    }
}

bool MimeService::setDefault(const std::string& mime, const std::string& app_id) noexcept {
    // Promote app_id to the front of the handler list
    auto it = registry_.find(mime);
    if (it == registry_.end()) return false;
    auto eit = std::find_if(it->second.begin(), it->second.end(),
        [&app_id](const AppEntry& e){ return e.app_id == app_id; });
    if (eit == it->second.end()) return false;
    if (eit != it->second.begin()) std::rotate(it->second.begin(), eit, eit + 1);

    // Persist the override
    ConfigService::set("mime.default." + mime, app_id);
    std::cout << "[MimeService] SetDefault: " << mime << " → " << app_id << "\n";
    return true;
}

std::string MimeService::appName(const std::string& app_id) noexcept {
    auto it = app_names_.find(app_id);
    return it == app_names_.end() ? std::string{} : it->second;
}

} // namespace fbx::xserver
