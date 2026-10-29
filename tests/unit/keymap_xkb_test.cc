// The XKB rule names InputManager::setKeymap uses (evdev / pc105) produce
// the layout's characters with this system's xkeyboard-config data.

#include <gtest/gtest.h>
#include <xkbcommon/xkbcommon.h>
#include <memory>

namespace {

struct Kb {
    std::unique_ptr<xkb_context, decltype(&xkb_context_unref)> ctx{xkb_context_new(XKB_CONTEXT_NO_FLAGS), &xkb_context_unref};
    std::unique_ptr<xkb_keymap, decltype(&xkb_keymap_unref)>   km{nullptr, &xkb_keymap_unref};
    std::unique_ptr<xkb_state, decltype(&xkb_state_unref)>     st{nullptr, &xkb_state_unref};
    bool load(const char* layout, const char* variant = nullptr) {
        xkb_rule_names n{};
        n.rules = "evdev"; n.model = "pc105"; n.layout = layout; n.variant = variant;
        km.reset(xkb_keymap_new_from_names(ctx.get(), &n, XKB_KEYMAP_COMPILE_NO_FLAGS));
        if (!km) return false;
        st.reset(xkb_state_new(km.get()));
        return st != nullptr;
    }
    // evdev keycode → character, the way InputManager looks it up.
    uint32_t press(uint32_t key) {
        uint32_t cp = xkb_state_key_get_utf32(st.get(), key + 8);
        xkb_state_update_key(st.get(), key + 8, XKB_KEY_DOWN);
        return cp;
    }
    void release(uint32_t key) { xkb_state_update_key(st.get(), key + 8, XKB_KEY_UP); }
};

}  // namespace

TEST(KeymapXkb, UsMatchesTheOldTable) {
    Kb kb;
    ASSERT_TRUE(kb.load("us"));
    EXPECT_EQ(kb.press(16), U'q'); kb.release(16);
    kb.press(42);                                   // Shift
    EXPECT_EQ(kb.press(3), U'@'); kb.release(3);
    kb.release(42);
    EXPECT_EQ(kb.press(28), 0x0Du);                 // Enter is a control code → caller keeps '\n'
}

TEST(KeymapXkb, GermanSwapsYZAndHasUmlauts) {
    Kb kb;
    ASSERT_TRUE(kb.load("de"));
    EXPECT_EQ(kb.press(21), U'z'); kb.release(21);  // US "y" position
    EXPECT_EQ(kb.press(44), U'y'); kb.release(44);  // US "z" position
    EXPECT_EQ(kb.press(26), U'ü'); kb.release(26);  // US "[" position
}

TEST(KeymapXkb, CapsLockAndBogusLayouts) {
    Kb kb;
    ASSERT_TRUE(kb.load("us"));
    kb.press(58); kb.release(58);                   // Caps Lock on
    EXPECT_EQ(kb.press(30), U'A'); kb.release(30);
    Kb bad;
    EXPECT_FALSE(bad.load("no-such-layout"));
}
