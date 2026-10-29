#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace fbx::xserver {

// Form-style URL encoding: unreserved characters (A-Z a-z 0-9 - . _ ~) pass,
// space → '+', every other byte → %XX. The output contains no shell
// metacharacters, so it's also safe inside a quoted shell word.
inline std::string urlEncodeQuery(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out += static_cast<char>(c);
        else if (c == ' ') out += '+';
        else { out += '%'; out += kHex[c >> 4]; out += kHex[c & 0xF]; }
    }
    return out;
}

}  // namespace fbx::xserver
