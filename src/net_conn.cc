#include "xserver/net_conn.h"

#include <algorithm>
#include <cctype>

namespace fbx::xserver::net {

namespace {
std::vector<std::string_view> splitWs(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}
std::string upper(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return o;
}
}  // namespace

std::string normProto(std::string_view p) noexcept {
    std::string u = upper(p);
    if (u.rfind("TCP", 0) == 0) return "TCP";
    if (u.rfind("UDP", 0) == 0) return "UDP";
    return u;
}

std::string abbrevState(std::string_view full) noexcept {
    if (full == "ESTABLISHED") return "ESTABL";
    if (full == "TIME_WAIT")   return "TIMEWT";
    if (full == "CLOSE_WAIT")  return "CLOSWT";
    if (full == "SYN_SENT")    return "SYNSNT";
    if (full == "SYN_RCVD")    return "SYNRCV";
    if (full == "FIN_WAIT_1" || full == "FIN_WAIT_2") return "FINWT";
    if (full == "LAST_ACK")    return "LASTAK";
    if (full == "CLOSING")     return "CLOSNG";
    if (full == "LISTEN")      return "LISTEN";
    if (full.size() <= 6)      return std::string(full);
    return std::string(full.substr(0, 6));
}

uint32_t connId(std::string_view proto, std::string_view local,
                std::string_view remote) noexcept {
    uint32_t h = 2166136261u;
    auto mix = [&](std::string_view s) {
        for (char c : s) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
        h ^= 0xff; h *= 16777619u;   // field separator
    };
    mix(proto); mix(local); mix(remote);
    return h ? h : 1u;
}

std::vector<Conn> parseSockstat(std::string_view out) noexcept {
    std::vector<Conn> conns;
    size_t pos = 0;
    while (pos <= out.size()) {
        size_t nl = out.find('\n', pos);
        std::string_view line = out.substr(pos, nl == std::string_view::npos
                                                     ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? out.size() + 1 : nl + 1;

        auto t = splitWs(line);
        // Columns: USER COMMAND PID FD PROTO LOCAL FOREIGN [STATE]
        if (t.size() < 7) continue;
        if (t[0] == "USER") continue;                    // header
        std::string proto = normProto(t[4]);
        if (proto != "TCP" && proto != "UDP") continue;  // skip UNIX sockets etc.

        Conn c;
        c.command = std::string(t[1]);
        c.pid     = static_cast<uint32_t>(std::atoi(std::string(t[2]).c_str()));
        c.proto   = proto;
        c.local   = std::string(t[5]);
        std::string foreign(t[6]);
        bool listening = (foreign == "*:*" || foreign.empty());
        c.remote = listening ? "-" : foreign;
        if (t.size() >= 8)      c.state = std::string(t[7]);
        else if (listening)     c.state = "LISTEN";
        conns.push_back(std::move(c));
    }
    return conns;
}

}  // namespace fbx::xserver::net
