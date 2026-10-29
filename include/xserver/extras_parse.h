#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// Pure helpers for com.freebsdx.Hotspot (can this Wi-Fi adapter be an access
// point?) and com.freebsdx.FileSync (Syncthing's config.xml and REST JSON).
// Dependency-free / unit-testable.
namespace fbx::xserver::extras {

// `ifconfig wlanN list caps`: "drivercaps=580c201<STA,PMGT,…>" — does the
// driver support access-point (HOSTAP) mode?
inline bool driverSupportsHostap(std::string_view caps) {
    size_t p = caps.find("drivercaps=");
    if (p == std::string_view::npos) return false;
    size_t lt = caps.find('<', p), gt = caps.find('>', p);
    if (lt == std::string_view::npos || gt == std::string_view::npos || gt < lt) return false;
    std::string_view list = caps.substr(lt + 1, gt - lt - 1);
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string_view::npos) comma = list.size();
        if (list.substr(start, comma - start) == "HOSTAP") return true;
        start = comma + 1;
    }
    return false;
}

// ── Syncthing ────────────────────────────────────────────────────────────────

struct SyncFolder {
    std::string id, label, path;
    bool        paused = false;
};

// An attribute value inside one XML start tag, or "".
inline std::string xmlAttr(std::string_view tag, std::string_view name) {
    std::string needle = " " + std::string(name) + "=\"";
    size_t p = tag.find(needle);
    if (p == std::string_view::npos) return {};
    p += needle.size();
    size_t q = tag.find('"', p);
    return q == std::string_view::npos ? std::string{} : std::string(tag.substr(p, q - p));
}

// config.xml: <folder id="abcd-1234" label="Docs" path="/home/u/Docs" … paused="false">
// (the per-folder <paused> child element is honoured too), and <apikey>…</apikey>.
inline std::vector<SyncFolder> parseFolders(std::string_view xml) {
    std::vector<SyncFolder> out;
    size_t pos = 0;
    while ((pos = xml.find("<folder ", pos)) != std::string_view::npos) {
        size_t end = xml.find('>', pos);
        if (end == std::string_view::npos) break;
        std::string_view tag = xml.substr(pos, end - pos);
        SyncFolder f{xmlAttr(tag, "id"), xmlAttr(tag, "label"), xmlAttr(tag, "path"), false};
        size_t close = xml.find("</folder>", end);
        std::string_view body = xml.substr(end, close == std::string_view::npos ? 0 : close - end);
        f.paused = xmlAttr(tag, "paused") == "true" || body.find("<paused>true</paused>") != std::string_view::npos;
        if (f.label.empty()) f.label = f.id;
        if (!f.id.empty()) out.push_back(std::move(f));
        pos = end;
    }
    return out;
}

inline std::string parseApiKey(std::string_view xml) {
    size_t p = xml.find("<apikey>");
    if (p == std::string_view::npos) return {};
    p += 8;
    size_t q = xml.find("</apikey>", p);
    return q == std::string_view::npos ? std::string{} : std::string(xml.substr(p, q - p));
}

// A top-level string or number from a flat JSON object ("state":"idle",
// "needBytes":123). Enough for Syncthing's /rest/db/status.
inline std::string jsonField(std::string_view json, std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t p = json.find(needle);
    if (p == std::string_view::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string_view::npos) return {};
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n')) ++p;
    if (p < json.size() && json[p] == '"') {
        size_t q = json.find('"', p + 1);
        return q == std::string_view::npos ? std::string{} : std::string(json.substr(p + 1, q - p - 1));
    }
    size_t q = p;
    while (q < json.size() && (std::isdigit(static_cast<unsigned char>(json[q])) || json[q] == '-' || json[q] == '.')) ++q;
    return std::string(json.substr(p, q - p));
}

// FileSyncEntry state (0 up-to-date, 1 syncing, 2 paused, 3 error) and
// progress from a folder's /rest/db/status.
inline void folderState(std::string_view status_json, bool paused, uint32_t& state, uint32_t& progress) {
    progress = 100;
    if (paused) { state = 2; return; }
    std::string st = jsonField(status_json, "state");
    if (st.empty() || st == "error") { state = 3; progress = 0; return; }
    double global = std::atof(jsonField(status_json, "globalBytes").c_str());
    double need   = std::atof(jsonField(status_json, "needBytes").c_str());
    if (global > 0) progress = static_cast<uint32_t>(100.0 * (global - need) / global);
    state = (st == "idle" && need <= 0) ? 0u : 1u;
}

// A folder id Syncthing generates / accepts in a URL path: [A-Za-z0-9._-].
inline bool safeFolderId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

}  // namespace fbx::xserver::extras
