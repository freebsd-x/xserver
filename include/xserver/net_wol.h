#pragma once

#include <array>
#include <cctype>
#include <cstdint>
#include <string_view>

// Pure Wake-on-LAN helpers: parse a MAC and build the 102-byte magic packet.
// Dependency-free so they can be unit-tested without opening a socket.
namespace fbx::xserver::wol {

// Parse "AA:BB:CC:DD:EE:FF" (or '-' separators) into 6 bytes. Returns false on
// any malformed input (wrong length, bad hex, wrong separator count).
inline bool parseMac(std::string_view s, uint8_t out[6]) noexcept {
    auto hex = [](char c, int& v) {
        if (c >= '0' && c <= '9') { v = c - '0'; return true; }
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
        return false;
    };
    size_t i = 0;
    for (int byte = 0; byte < 6; ++byte) {
        if (i + 2 > s.size()) return false;          // need two hex digits
        int hi, lo;
        if (!hex(s[i], hi) || !hex(s[i + 1], lo)) return false;
        out[byte] = static_cast<uint8_t>((hi << 4) | lo);
        i += 2;
        if (byte < 5) {                               // separator after each but last
            if (i >= s.size() || (s[i] != ':' && s[i] != '-')) return false;
            ++i;
        }
    }
    return i == s.size();                              // no trailing junk
}

// 6 bytes of 0xFF followed by the target MAC repeated 16 times = 102 bytes.
inline std::array<uint8_t, 102> buildMagicPacket(const uint8_t mac[6]) noexcept {
    std::array<uint8_t, 102> pkt{};
    for (int i = 0; i < 6; ++i) pkt[i] = 0xFF;
    for (int r = 0; r < 16; ++r)
        for (int b = 0; b < 6; ++b) pkt[6 + r * 6 + b] = mac[b];
    return pkt;
}

}  // namespace fbx::xserver::wol
