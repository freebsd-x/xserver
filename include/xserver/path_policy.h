#pragma once
// Where the compositor will touch a path on an app's behalf. It runs outside
// every sandbox, so a path it's handed is only safe once resolved: comparing the
// text lets "/tmp/../home/me/.ssh" and a symlink out of the folder through.

#include <climits>
#include <cstdlib>
#include <initializer_list>
#include <string>

namespace fbx::xserver::pathpolicy {

// Is `path` (already resolved) `root` itself or inside it? A shared prefix isn't
// enough: /home/mean isn't inside /home/me.
inline bool inside(const std::string& path, const std::string& root) {
    if (root.empty()) return false;
    if (root == "/") return !path.empty() && path[0] == '/';
    return path == root || path.compare(0, root.size() + 1, root + "/") == 0;
}

// `path` with every "..", "." and symlink resolved if it exists and lands in one
// of `roots`, else "". Use the result, not the original text, from then on.
inline std::string resolveWithin(const std::string& path, std::initializer_list<const char*> roots) {
    if (path.empty() || path.size() >= PATH_MAX) return {};
    char real[PATH_MAX];
    if (!::realpath(path.c_str(), real)) return {};
    for (const char* root : roots) {
        char real_root[PATH_MAX];
        if (root && *root && ::realpath(root, real_root) && inside(real, real_root)) return real;
    }
    return {};
}

}  // namespace fbx::xserver::pathpolicy
