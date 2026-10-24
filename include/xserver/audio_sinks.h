#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cctype>

// Pure parser for /dev/sndstat (com.freebsdx.AudioOutput). Lists play-capable
// pcm devices (output sinks); the "default" marker is the active sink (reflects
// hw.snd.default_unit). Dependency-free / unit-testable.
//
//   pcm0: <Realtek ALC294 (Analog 2.0+HP/4ch)> (play/rec) default
//   pcm1: <Intel Kaby Lake (HDMI/DP 8ch)> (play)
namespace fbx::xserver::audio {

struct AudioSink {
    std::string id;       // "pcm0"
    std::string name;     // "Realtek ALC294 (Analog 2.0+HP/4ch)"
    bool        active = false;
};

// Unit number from a "pcmN" id (N); UINT32_MAX if malformed.
inline uint32_t unitOfId(std::string_view id) {
    if (id.substr(0, 3) != "pcm") return UINT32_MAX;
    uint32_t v = 0; bool any = false;
    for (size_t i = 3; i < id.size() && id[i] >= '0' && id[i] <= '9'; ++i) {
        v = v * 10 + static_cast<uint32_t>(id[i] - '0'); any = true;
    }
    return any ? v : UINT32_MAX;
}

inline std::vector<AudioSink> parseSndstat(std::string_view out) {
    std::vector<AudioSink> v;
    size_t start = 0;
    while (true) {
        size_t nl = out.find('\n', start);
        std::string_view line = out.substr(start, (nl == std::string_view::npos ? out.size() : nl) - start);

        size_t i = 0;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (line.substr(i, 3) == "pcm") {
            size_t j = i + 3; uint32_t unit = 0; bool any = false;
            while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
                unit = unit * 10 + static_cast<uint32_t>(line[j] - '0'); ++j; any = true;
            }
            if (any && j < line.size() && line[j] == ':') {
                // name = <...>; the device description may itself contain "(...)".
                size_t lt = line.find('<', j);
                size_t gt = (lt == std::string_view::npos) ? std::string_view::npos : line.find('>', lt);
                std::string name = (lt != std::string_view::npos && gt != std::string_view::npos && gt > lt)
                                       ? std::string(line.substr(lt + 1, gt - lt - 1)) : std::string{};
                // capability + flags live after the closing '>'.
                std::string_view tail = (gt != std::string_view::npos) ? line.substr(gt + 1) : std::string_view{};
                if (tail.find("play") != std::string_view::npos) {   // output sinks only
                    AudioSink s;
                    s.id     = "pcm" + std::to_string(unit);
                    s.name   = name;
                    s.active = (tail.find("default") != std::string_view::npos);
                    v.push_back(std::move(s));
                }
            }
        }
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return v;
}

}  // namespace fbx::xserver::audio
