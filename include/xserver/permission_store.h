#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace fbx::xserver {

// Per-app capability grants for the runtime permission system. Keyed by app
// identifier (SecurityContextInfo.app_id_, e.g. "com.freebsdx.Camera"); each app
// has one bit per capability. Grants are set by the user (AppSandbox settings
// panel) and persisted to ~/.freebsdx/app-permissions so they survive restarts.
//
// The dispatcher consults isAllowed() before letting a *sandboxed* app use a
// sensitive capability. Default policy when an app has no stored decision is per
// capability: sensitive ones (camera, microphone) default DENY (deny-by-default
// consent model — the app stays blocked until the user grants it); the rest
// default ALLOW to preserve existing behaviour. Trusted (non-sandboxed) apps are
// not gated here — the dispatcher checks isSandboxed() first.
class PermissionStore {
public:
    enum Capability : uint32_t {
        Net = 0, Camera = 1, Microphone = 2, Filesystem = 3, Gpu = 4, ScreenCapture = 5
    };
    static constexpr uint32_t kNumCaps = 6;

    // Pure decision: is `cap` allowed for `app`, given stored grants + defaults?
    bool isAllowed(const std::string& app, Capability cap) const noexcept;

    // Record a user decision (grant/revoke) and persist. Returns false on a bad
    // capability index.
    bool set(const std::string& app, Capability cap, bool allowed) noexcept;

    struct AppPerms {
        std::string app;
        std::array<bool, kNumCaps> allowed;
    };
    std::vector<AppPerms> list() const noexcept;   // sorted by app id

    // Persistence. path defaults to ~/.freebsdx/app-permissions.
    void load(const std::string& path = "") noexcept;
    bool save() const noexcept;

    // Default grant for a capability when no explicit decision exists.
    static bool defaultFor(Capability cap) noexcept {
        // Sensitive capabilities deny by default (deny-by-default consent model).
        return !(cap == Camera || cap == Microphone || cap == ScreenCapture);
    }

private:
    std::string resolvePath(const std::string& path) const;

    // An app's record: which caps have an explicit decision, and its value.
    struct Rec { std::array<bool, kNumCaps> set{}; std::array<bool, kNumCaps> val{}; };

    mutable std::mutex mutex_;
    std::map<std::string, Rec> apps_;
    std::string path_;
};

}  // namespace fbx::xserver
