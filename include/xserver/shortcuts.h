#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

// The desktop's global keyboard shortcuts, as handled by InputManager and the
// compositor's key path. com.freebsdx.KeyboardShortcuts lists them and can
// switch any off; each handler checks enabled() so a disabled shortcut's keys
// go to the focused app instead. (Ctrl+Alt+Esc emergency exit and the media/
// brightness keys aren't listed and can't be disabled.)
namespace fbx::xserver::shortcuts {

enum Id : uint32_t {
    kSearch, kDashboard, kClipboard, kColorPicker, kQuickNote, kHelp, kScreenshot,
    kLock, kClose, kTerminal, kSwitchWindows, kWorkspaces, kSnap, kTiling, kDebugOverlay,
    kCount
};

struct Def {
    Id          id;
    const char* action;    // ≤ 47 chars (KeyShortcutPage::action_)
    const char* binding;   // ≤ 15 chars (KeyShortcutPage::binding_)
};

inline constexpr Def kAll[kCount] = {
    {kSearch,        "Search",                         "Super+Space"},
    {kDashboard,     "Dashboard",                      "Super+C"},
    {kClipboard,     "Clipboard history",              "Super+V"},
    {kColorPicker,   "Color picker",                   "Super+P"},
    {kQuickNote,     "Quick note",                     "Super+N"},
    {kHelp,          "Shortcut help",                  "Super+/"},
    {kScreenshot,    "Screenshot (Shift: region)",     "PrtSc"},
    {kLock,          "Lock screen",                    "Super+L"},
    {kClose,         "Close window",                   "Super+Q"},
    {kTerminal,      "Open terminal",                  "Super+Enter"},
    {kSwitchWindows, "Switch windows",                 "Alt+Tab"},
    {kWorkspaces,    "Switch workspace",               "Super+1..9"},
    {kSnap,          "Snap / maximize / move window",  "Super+Arrows"},
    {kTiling,        "Tiling (layout, resize, focus)", "Super+A/[/]/M"},
    {kDebugOverlay,  "GPU frame-time overlay",         "Super+F3"},
};

// Bit i set = shortcut i disabled. Written by KeyboardShortcutsService, read
// on the input path.
inline std::atomic<uint32_t> g_disabled{0};

inline bool enabled(Id id) { return (g_disabled.load(std::memory_order_relaxed) & (1u << id)) == 0; }

inline void setEnabled(Id id, bool on) {
    if (on) g_disabled.fetch_and(~(1u << id));
    else    g_disabled.fetch_or(1u << id);
}

// The shortcut with this action name, or kCount.
inline Id byAction(std::string_view action) {
    for (const auto& d : kAll) if (action == d.action) return d.id;
    return kCount;
}

}  // namespace fbx::xserver::shortcuts
