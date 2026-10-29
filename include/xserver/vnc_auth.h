#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

#include <openssl/crypto.h>
#include <openssl/des.h>

// VNC Authentication (RFB security type 2): the client DES-ECB-encrypts the
// server's 16-byte challenge with the password as the key — its first 8
// bytes, zero-padded, each byte bit-reversed (the classic VNC quirk). Uses
// OpenSSL's low-level DES (deprecated in 3.x but present, and unlike EVP
// "DES-ECB" it doesn't need the legacy provider).
namespace fbx::xserver::vnc {

inline uint8_t reverseBits(uint8_t b) {
    b = static_cast<uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = static_cast<uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = static_cast<uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

// The response a client knowing `password` sends for `challenge`.
inline void expectedResponse(std::string_view password, const uint8_t challenge[16], uint8_t out[16]) {
    DES_cblock key{};
    for (size_t i = 0; i < 8 && i < password.size(); ++i)
        key[i] = reverseBits(static_cast<uint8_t>(password[i]));
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    DES_key_schedule ks;
    DES_set_key_unchecked(&key, &ks);
    for (int blk = 0; blk < 2; ++blk) {
        DES_cblock in, enc;
        std::memcpy(in, challenge + blk * 8, 8);
        DES_ecb_encrypt(&in, &enc, &ks, DES_ENCRYPT);
        std::memcpy(out + blk * 8, enc, 8);
    }
#pragma GCC diagnostic pop
    OPENSSL_cleanse(&ks, sizeof(ks));
    OPENSSL_cleanse(key, sizeof(key));
}

// Constant-time check of a client's response.
inline bool verify(std::string_view password, const uint8_t challenge[16], const uint8_t response[16]) {
    if (password.empty()) return false;
    uint8_t want[16];
    expectedResponse(password, challenge, want);
    bool ok = CRYPTO_memcmp(want, response, 16) == 0;
    OPENSSL_cleanse(want, sizeof(want));
    return ok;
}

}  // namespace fbx::xserver::vnc
