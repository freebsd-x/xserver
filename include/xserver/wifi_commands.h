#pragma once

#include <openssl/evp.h>

#include <cstdio>
#include <string>
#include <vector>

// The wpa_supplicant commands that join a Wi-Fi network.
//
// The old code shell-quoted the SSID and passphrase but passed them to wpa_cli
// bare: set_network takes a string value *with* double quotes ('"MyWifi"');
// unquoted, it is read as hex — so a normal SSID was rejected — and it always
// configured network 0 instead of the one add_network created. Here the SSID
// goes as hex (no quoting can be wrong), the passphrase is turned into the
// 256-bit PSK in-process (PBKDF2-HMAC-SHA1, 4096 rounds, salt = SSID — what
// wpa_passphrase does), and the commands go over the control socket
// (wpa_ctrl.h), so neither ever appears on a command line.
namespace fbx::xserver::wifi {

inline std::string hex(const unsigned char* p, size_t n) {
    std::string out;
    out.reserve(n * 2);
    char b[3];
    for (size_t i = 0; i < n; ++i) { std::snprintf(b, sizeof(b), "%02x", p[i]); out += b; }
    return out;
}

inline std::string ssidHex(const std::string& ssid) {
    return hex(reinterpret_cast<const unsigned char*>(ssid.data()), ssid.size());
}

// The WPA-PSK for `passphrase` on `ssid`, as 64 hex digits; "" if the
// passphrase isn't a valid WPA one (8-63 printable ASCII characters).
inline std::string pskHex(const std::string& passphrase, const std::string& ssid) {
    if (passphrase.size() < 8 || passphrase.size() > 63) return {};
    for (unsigned char c : passphrase)
        if (c < 0x20 || c > 0x7e) return {};
    unsigned char psk[32];
    if (PKCS5_PBKDF2_HMAC_SHA1(passphrase.data(), static_cast<int>(passphrase.size()),
                               reinterpret_cast<const unsigned char*>(ssid.data()),
                               static_cast<int>(ssid.size()), 4096, sizeof(psk), psk) != 1)
        return {};
    return hex(psk, sizeof(psk));
}

// ADD_NETWORK replies with the new network's id: digits only.
inline bool validNetworkId(const std::string& id) {
    if (id.empty() || id.size() > 5) return false;
    for (char c : id)
        if (c < '0' || c > '9') return false;
    return true;
}

// The control-interface commands (wpa_ctrl.h) that configure network `id`
// (from ADD_NETWORK) and switch to it. Empty if the passphrase is invalid. An
// empty passphrase means an open network.
inline std::vector<std::string> joinCommands(const std::string& id, const std::string& ssid,
                                             const std::string& passphrase) {
    std::vector<std::string> cmds;
    cmds.push_back("SET_NETWORK " + id + " ssid " + ssidHex(ssid));
    if (passphrase.empty()) {
        cmds.push_back("SET_NETWORK " + id + " key_mgmt NONE");
    } else {
        std::string psk = pskHex(passphrase, ssid);
        if (psk.empty()) return {};
        cmds.push_back("SET_NETWORK " + id + " psk " + psk);
    }
    cmds.push_back("ENABLE_NETWORK " + id);
    cmds.push_back("SELECT_NETWORK " + id);
    return cmds;
}

}  // namespace fbx::xserver::wifi
