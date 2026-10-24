#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Pure parser for saved Wi-Fi networks from a wpa_supplicant.conf body. Reads
// the `network={ … }` blocks into a neutral list; dependency-free + testable.
namespace fbx::xserver::wifi {

struct SavedNet {
    std::string ssid;
    uint32_t    security = 1;      // 0=open, 1=WPA2, 2=WPA3
    uint32_t    auto_connect = 1;  // 0 when the block is disabled=1
};

// Classify from a network block's key_mgmt value.
inline uint32_t securityFromKeyMgmt(std::string_view km) noexcept {
    if (km.find("SAE") != std::string_view::npos) return 2;   // WPA3
    if (km.find("NONE") != std::string_view::npos) return 0;  // open
    if (km.find("WPA") != std::string_view::npos ||
        km.find("EAP") != std::string_view::npos) return 1;   // WPA2/enterprise
    return 1;                                                  // default assume secured
}

namespace detail {
// Value of `key=...` (or key="...") within a block; "" if absent.
inline std::string field(std::string_view block, std::string_view key) {
    size_t p = 0;
    while ((p = block.find(key, p)) != std::string_view::npos) {
        // ensure it's at a token boundary (start of line-ish / preceded by ws)
        bool boundary = (p == 0) || block[p - 1] == '\n' || block[p - 1] == ' ' ||
                        block[p - 1] == '\t';
        size_t after = p + key.size();
        if (boundary && after < block.size() && block[after] == '=') {
            size_t v = after + 1;
            if (v < block.size() && block[v] == '"') {
                size_t end = block.find('"', v + 1);
                if (end != std::string_view::npos)
                    return std::string(block.substr(v + 1, end - v - 1));
            }
            size_t end = v;
            while (end < block.size() && block[end] != '\n' && block[end] != ' ' &&
                   block[end] != '\t' && block[end] != '}')
                ++end;
            return std::string(block.substr(v, end - v));
        }
        p = after;
    }
    return "";
}
}  // namespace detail

inline std::vector<SavedNet> parseWpaSupplicant(std::string_view text) noexcept {
    std::vector<SavedNet> out;
    size_t pos = 0;
    while ((pos = text.find("network=", pos)) != std::string_view::npos) {
        size_t brace = text.find('{', pos);
        if (brace == std::string_view::npos) break;
        size_t end = text.find('}', brace);
        if (end == std::string_view::npos) break;
        std::string_view block = text.substr(brace + 1, end - brace - 1);
        pos = end + 1;

        std::string ssid = detail::field(block, "ssid");
        if (ssid.empty()) continue;   // skip blocks without an SSID
        SavedNet n;
        n.ssid = ssid;
        n.security = securityFromKeyMgmt(detail::field(block, "key_mgmt"));
        n.auto_connect = (detail::field(block, "disabled") == "1") ? 0u : 1u;
        out.push_back(std::move(n));
    }
    return out;
}

}  // namespace fbx::xserver::wifi
