#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>

// TOTP (RFC 6238) for com.freebsdx.Totp: otpauth:// URI parsing, base32
// secrets, and code generation (HMAC via libcrypto). Accounts live one URI per
// line in ~/.config/freebsdx/totp (mode 0600, read by the service).
namespace fbx::xserver::totp {

struct Account {
    std::string issuer;
    std::string account;
    std::vector<uint8_t> secret;
    std::string algorithm = "SHA1";   // SHA1 | SHA256 | SHA512
    uint32_t    digits = 6;           // 6-8
    uint32_t    period = 30;          // seconds
};

// RFC 4648 base32, case-insensitive, '=' padding and spaces ignored.
inline bool base32Decode(std::string_view in, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == ' ') continue;
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a';
        else if (c >= '2' && c <= '7') v = c - '2' + 26;
        else return false;
        buf = (buf << 5) | static_cast<uint32_t>(v);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xff));
        }
    }
    return !out.empty();
}

inline std::string percentDecode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            unsigned v = 0;
            if (std::sscanf(std::string(s.substr(i + 1, 2)).c_str(), "%2x", &v) == 1) {
                out += static_cast<char>(v);
                i += 2;
                continue;
            }
        }
        out += (s[i] == '+') ? ' ' : s[i];
    }
    return out;
}

// otpauth://totp/[Issuer:]account?secret=…[&issuer=…][&algorithm=…][&digits=…][&period=…]
// hotp:// is rejected (it needs a persisted counter).
inline bool parseUri(std::string_view uri, Account& a) {
    constexpr std::string_view kPrefix = "otpauth://totp/";
    if (uri.substr(0, kPrefix.size()) != kPrefix) return false;
    uri.remove_prefix(kPrefix.size());
    size_t q = uri.find('?');
    std::string label = percentDecode(uri.substr(0, q));
    size_t colon = label.find(':');
    if (colon != std::string::npos) {
        a.issuer  = label.substr(0, colon);
        a.account = label.substr(colon + 1);
        while (!a.account.empty() && a.account[0] == ' ') a.account.erase(0, 1);
    } else {
        a.account = label;
    }
    bool have_secret = false;
    std::string_view params = q == std::string_view::npos ? std::string_view{} : uri.substr(q + 1);
    while (!params.empty()) {
        size_t amp = params.find('&');
        std::string_view kv = params.substr(0, amp);
        params = amp == std::string_view::npos ? std::string_view{} : params.substr(amp + 1);
        size_t eq = kv.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view k = kv.substr(0, eq);
        std::string v = percentDecode(kv.substr(eq + 1));
        if (k == "secret")         have_secret = base32Decode(v, a.secret);
        else if (k == "issuer")    a.issuer = v;
        else if (k == "algorithm") a.algorithm = v;
        else if (k == "digits")    a.digits = static_cast<uint32_t>(std::atoi(v.c_str()));
        else if (k == "period")    a.period = static_cast<uint32_t>(std::atoi(v.c_str()));
    }
    for (char& c : a.algorithm) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return have_secret && a.digits >= 6 && a.digits <= 8 && a.period >= 1 && a.period <= 300 &&
           (a.algorithm == "SHA1" || a.algorithm == "SHA256" || a.algorithm == "SHA512");
}

// One URI per line; blanks, '#' comments and invalid lines skipped.
inline std::vector<Account> parseFile(std::string_view text) {
    std::vector<Account> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(start, nl - start);
        start = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        Account a;
        if (!line.empty() && line[0] != '#' && parseUri(line, a)) out.push_back(std::move(a));
    }
    return out;
}

// Stable id: 8 hex chars of FNV-1a over "issuer:account".
inline std::string accountId(const Account& a) {
    uint32_t h = 2166136261u;
    for (char c : a.issuer + ":" + a.account) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", h);
    return buf;
}

// RFC 4226 HOTP value for `counter`, zero-padded to a.digits.
inline std::string hotp(const Account& a, uint64_t counter) {
    uint8_t msg[8];
    for (int i = 7; i >= 0; --i) { msg[i] = static_cast<uint8_t>(counter & 0xff); counter >>= 8; }
    const EVP_MD* md = a.algorithm == "SHA256" ? EVP_sha256()
                     : a.algorithm == "SHA512" ? EVP_sha512() : EVP_sha1();
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (!HMAC(md, a.secret.data(), static_cast<int>(a.secret.size()), msg, sizeof(msg), mac, &len) || len < 20)
        return {};
    unsigned off = mac[len - 1] & 0x0f;
    uint32_t bin = (static_cast<uint32_t>(mac[off] & 0x7f) << 24) | (static_cast<uint32_t>(mac[off + 1]) << 16) |
                   (static_cast<uint32_t>(mac[off + 2]) << 8) | mac[off + 3];
    uint32_t mod = 1;
    for (uint32_t i = 0; i < a.digits; ++i) mod *= 10;
    std::string code = std::to_string(bin % mod);
    return std::string(a.digits - code.size(), '0') + code;
}

inline std::string code(const Account& a, std::time_t now) {
    return hotp(a, static_cast<uint64_t>(now) / a.period);
}

inline uint32_t secondsRemaining(const Account& a, std::time_t now) {
    return a.period - static_cast<uint32_t>(static_cast<uint64_t>(now) % a.period);
}

}  // namespace fbx::xserver::totp
