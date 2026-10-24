#pragma once

#include <cstdlib>
#include <fstream>
#include <string>

// Tiny persistence helpers shared by the simple toggle services (AirplaneMode,
// DataSaver, DoNotDisturb, NightLight): each stores its state as whitespace-
// separated ints under ~/.freebsdx-<name>.
namespace fbx::xserver::toggle {

inline std::string path(const char* name) {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.freebsdx-" + name;
}

// Load up to N ints from the file into out[]; missing values keep their defaults.
inline void loadInts(const char* name, uint32_t* out, int n) {
    std::ifstream f(path(name));
    for (int i = 0; i < n && (f >> out[i]); ++i) {}
}

inline void saveInts(const char* name, const uint32_t* v, int n) {
    std::ofstream f(path(name));
    for (int i = 0; i < n; ++i) f << v[i] << (i + 1 < n ? ' ' : '\n');
}

}  // namespace fbx::xserver::toggle
