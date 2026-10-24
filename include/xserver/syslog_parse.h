#pragma once

#include <string>
#include <string_view>
#include <vector>

// Pure parser for a traditional syslog line for com.freebsdx.SysLog:
//   "Sep 20 14:30:01 host sshd[123]: Accepted password for ..."
// → facility "sshd", message "Accepted password for ...". Dependency-free /
// unit-testable (reading /var/log lives in the service).
namespace fbx::xserver::syslog {

struct Entry {
    std::string facility;
    std::string message;
    uint32_t    level = 6;   // default: info (traditional lines carry no severity)
};

namespace detail {
inline std::vector<std::string_view> split(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        size_t start = i;
        while (i < s.size() && s[i] != ' ') ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}
}  // namespace detail

inline Entry parseLine(std::string_view line) noexcept {
    // strip trailing CR/newline
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.remove_suffix(1);

    auto t = detail::split(line);
    // timestamp(3) + host(1) + tag(1) + message → need at least 6 tokens
    if (t.size() < 6) return Entry{"", std::string(line), 6};

    std::string tag(t[4]);
    if (auto br = tag.find('['); br != std::string::npos) tag.resize(br);  // drop [pid]
    if (!tag.empty() && tag.back() == ':') tag.pop_back();                 // drop trailing ':'

    std::string msg;
    for (size_t i = 5; i < t.size(); ++i) {
        if (i > 5) msg += ' ';
        msg += std::string(t[i]);
    }
    return Entry{tag, msg, 6};
}

}  // namespace fbx::xserver::syslog
