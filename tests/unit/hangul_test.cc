// 2-beolsik Hangul composition, typed as QWERTY key sequences.

#include "xserver/hangul.h"
#include <gtest/gtest.h>
#include <string>

using fbx::xserver::hangul::Composer;

namespace {
// Everything that ends up committed after typing `keys` and flushing.
std::string type(std::string_view keys) {
    Composer c;
    std::string out;
    for (char k : keys) out += c.feed(k);
    return out + c.flush();
}
}  // namespace

TEST(Hangul, Words) {
    EXPECT_EQ(type("dkssudgktpdy"), "안녕하세요");
    EXPECT_EQ(type("gksrmf"), "한글");
    EXPECT_EQ(type("rkatkgkqslek"), "감사합니다");
    EXPECT_EQ(type("tkfkd"), "사랑");
}

TEST(Hangul, FinalMovesToNextSyllable) {
    EXPECT_EQ(type("dksu"), "아녀");          // 안 + ㅕ → 아 + 녀
    EXPECT_EQ(type("dlfrdj"), "읽어");        // compound final ㄺ stays before ㅇ
    EXPECT_EQ(type("dlfrj"), "일거");         // ...but splits before a vowel
    EXPECT_EQ(type("rkqt"), "값");
    EXPECT_EQ(type("rkqtdl"), "값이");
}

TEST(Hangul, CompoundVowelsAndShift) {
    EXPECT_EQ(type("dhk"), "와");
    EXPECT_EQ(type("rhkd"), "광");
    EXPECT_EQ(type("dnl"), "위");
    EXPECT_EQ(type("dml"), "의");
    EXPECT_EQ(type("Tks"), "싼");
    EXPECT_EQ(type("RkR"), "깎");             // ㄲ can be a final
    EXPECT_EQ(type("dkE"), "아ㄸ");           // ㄸ can't
    EXPECT_EQ(type("dP"), "예");
}

TEST(Hangul, LoneJamo) {
    EXPECT_EQ(type("k"), "ㅏ");
    EXPECT_EQ(type("rr"), "ㄱㄱ");
    EXPECT_EQ(type("kr"), "ㅏㄱ");
}

TEST(Hangul, PreeditAndBackspace) {
    Composer c;
    for (char k : std::string("dks")) EXPECT_EQ(c.feed(k), "");
    EXPECT_EQ(c.preedit(), "안");
    EXPECT_TRUE(c.backspace());
    EXPECT_EQ(c.preedit(), "아");
    EXPECT_TRUE(c.backspace());
    EXPECT_EQ(c.preedit(), "ㅇ");
    EXPECT_TRUE(c.backspace());
    EXPECT_TRUE(c.empty());
    EXPECT_FALSE(c.backspace());

    for (char k : std::string("dhk")) c.feed(k);
    EXPECT_EQ(c.preedit(), "와");
    EXPECT_TRUE(c.backspace());
    EXPECT_EQ(c.preedit(), "오");             // compound vowel loses its second half
    c.flush();

    for (char k : std::string("dlfr")) c.feed(k);
    EXPECT_EQ(c.preedit(), "읽");
    EXPECT_TRUE(c.backspace());
    EXPECT_EQ(c.preedit(), "일");
}

TEST(Hangul, NonHangulKeyFlushes) {
    Composer c;
    c.feed('g');
    c.feed('k');
    EXPECT_EQ(c.feed('1'), "하");
    EXPECT_TRUE(c.empty());
}
