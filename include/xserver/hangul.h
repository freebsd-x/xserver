#pragma once

#include <cstdint>
#include <string>

// Korean 2-beolsik (두벌식) composer for the compositor IME (locale 3).
// Keys are the QWERTY characters the keyboard produces ('r' → ㄱ, 'R' → ㄲ, …).
// feed() returns text to commit (finished syllables); preedit() is the
// syllable being built. Dependency-free / unit-testable.
namespace fbx::xserver::hangul {

inline std::string utf8(uint32_t cp) {
    std::string s;
    if (cp < 0x80) s += static_cast<char>(cp);
    else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

// Hangul Compatibility Jamo for a key, or 0. Consonants 0x3131-0x314E,
// vowels 0x314F-0x3163.
inline uint32_t jamoForKey(char k) {
    switch (k) {
    case 'r': return 0x3131; case 'R': return 0x3132;  // ㄱ ㄲ
    case 's': case 'S': return 0x3134;                  // ㄴ
    case 'e': return 0x3137; case 'E': return 0x3138;  // ㄷ ㄸ
    case 'f': case 'F': return 0x3139;                  // ㄹ
    case 'a': case 'A': return 0x3141;                  // ㅁ
    case 'q': return 0x3142; case 'Q': return 0x3143;  // ㅂ ㅃ
    case 't': return 0x3145; case 'T': return 0x3146;  // ㅅ ㅆ
    case 'd': case 'D': return 0x3147;                  // ㅇ
    case 'w': return 0x3148; case 'W': return 0x3149;  // ㅈ ㅉ
    case 'c': case 'C': return 0x314A;                  // ㅊ
    case 'z': case 'Z': return 0x314B;                  // ㅋ
    case 'x': case 'X': return 0x314C;                  // ㅌ
    case 'v': case 'V': return 0x314D;                  // ㅍ
    case 'g': case 'G': return 0x314E;                  // ㅎ
    case 'k': case 'K': return 0x314F;                  // ㅏ
    case 'o': return 0x3150; case 'O': return 0x3152;  // ㅐ ㅒ
    case 'i': case 'I': return 0x3151;                  // ㅑ
    case 'j': case 'J': return 0x3153;                  // ㅓ
    case 'p': return 0x3154; case 'P': return 0x3156;  // ㅔ ㅖ
    case 'u': case 'U': return 0x3155;                  // ㅕ
    case 'h': case 'H': return 0x3157;                  // ㅗ
    case 'y': case 'Y': return 0x315B;                  // ㅛ
    case 'n': case 'N': return 0x315C;                  // ㅜ
    case 'b': case 'B': return 0x3160;                  // ㅠ
    case 'm': case 'M': return 0x3161;                  // ㅡ
    case 'l': case 'L': return 0x3163;                  // ㅣ
    default: return 0;
    }
}

inline bool isVowel(uint32_t j) { return j >= 0x314F && j <= 0x3163; }

namespace detail {
// Compatibility jamo → initial (choseong) index, or -1.
inline int choIndex(uint32_t j) {
    static constexpr uint32_t kCho[19] = {0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141,
                                          0x3142, 0x3143, 0x3145, 0x3146, 0x3147, 0x3148, 0x3149,
                                          0x314A, 0x314B, 0x314C, 0x314D, 0x314E};
    for (int i = 0; i < 19; ++i) if (kCho[i] == j) return i;
    return -1;
}
// Compatibility jamo → final (jongseong) index, or 0 (ㄸ ㅃ ㅉ can't end a syllable).
inline int jongIndex(uint32_t j) {
    static constexpr uint32_t kJong[28] = {0, 0x3131, 0x3132, 0x3133, 0x3134, 0x3135, 0x3136,
                                           0x3137, 0x3139, 0x313A, 0x313B, 0x313C, 0x313D, 0x313E,
                                           0x313F, 0x3140, 0x3141, 0x3142, 0x3144, 0x3145, 0x3146,
                                           0x3147, 0x3148, 0x314A, 0x314B, 0x314C, 0x314D, 0x314E};
    for (int i = 1; i < 28; ++i) if (kJong[i] == j) return i;
    return 0;
}
inline int jungIndex(uint32_t j) { return isVowel(j) ? static_cast<int>(j - 0x314F) : -1; }

// Compound vowels: ㅗ+ㅏ=ㅘ, …; 0 if they don't combine.
inline uint32_t combineVowel(uint32_t a, uint32_t b) {
    struct C { uint32_t a, b, r; };
    static constexpr C kV[] = {{0x3157, 0x314F, 0x3158}, {0x3157, 0x3150, 0x3159}, {0x3157, 0x3163, 0x315A},
                               {0x315C, 0x3153, 0x315D}, {0x315C, 0x3154, 0x315E}, {0x315C, 0x3163, 0x315F},
                               {0x3161, 0x3163, 0x3162}};
    for (const auto& c : kV) if (c.a == a && c.b == b) return c.r;
    return 0;
}
// Compound finals: ㄱ+ㅅ=ㄳ, …; 0 if they don't combine.
inline uint32_t combineFinal(uint32_t a, uint32_t b) {
    struct C { uint32_t a, b, r; };
    static constexpr C kF[] = {{0x3131, 0x3145, 0x3133}, {0x3134, 0x3148, 0x3135}, {0x3134, 0x314E, 0x3136},
                               {0x3139, 0x3131, 0x313A}, {0x3139, 0x3141, 0x313B}, {0x3139, 0x3142, 0x313C},
                               {0x3139, 0x3145, 0x313D}, {0x3139, 0x314C, 0x313E}, {0x3139, 0x314D, 0x313F},
                               {0x3139, 0x314E, 0x3140}, {0x3142, 0x3145, 0x3144}};
    for (const auto& c : kF) if (c.a == a && c.b == b) return c.r;
    return 0;
}
}  // namespace detail

class Composer {
public:
    // Feed one key; returns text to commit before the (new) preedit. Keys that
    // aren't Hangul return the finished composition and leave it empty — the
    // caller then passes the key through.
    std::string feed(char key) {
        uint32_t j = jamoForKey(key);
        if (!j) return flush();
        return isVowel(j) ? feedVowel(j) : feedConsonant(j);
    }

    // The syllable or lone jamo being composed (UTF-8), "" if none.
    [[nodiscard]] std::string preedit() const {
        if (!cho_ && !jung_ && !jong_) return {};
        if (cho_ && jung_) {
            uint32_t cp = 0xAC00 + (static_cast<uint32_t>(detail::choIndex(cho_)) * 21 +
                                    static_cast<uint32_t>(detail::jungIndex(jung_))) * 28 +
                          static_cast<uint32_t>(jong_ ? detail::jongIndex(jong_) : 0);
            return utf8(cp);
        }
        return utf8(cho_ ? cho_ : jung_);   // lone consonant or vowel
    }

    [[nodiscard]] bool empty() const { return !cho_ && !jung_ && !jong_; }

    // Remove the last-typed component; false if there was nothing to remove.
    bool backspace() {
        if (jong_) {
            jong_ = jong2_ ? jong1_ : 0;
            jong1_ = jong2_ = 0;
            if (jong_) jong1_ = jong_;
            return true;
        }
        if (jung_) {
            jung_ = jung2_ ? jung1_ : 0;
            jung1_ = jung2_ = 0;
            if (jung_) jung1_ = jung_;
            return true;
        }
        if (cho_) { cho_ = 0; return true; }
        return false;
    }

    // Commit what's being composed and start over.
    std::string flush() {
        std::string s = preedit();
        reset();
        return s;
    }

private:
    void reset() { cho_ = jung_ = jong_ = jung1_ = jung2_ = jong1_ = jong2_ = 0; }

    std::string feedVowel(uint32_t v) {
        if (jong_) {
            // The final consonant (or the second half of a compound final)
            // moves to the next syllable: 안 + ㅕ → 아 + 녀.
            uint32_t carry = jong2_ ? jong2_ : jong_;
            jong_ = jong2_ ? jong1_ : 0;
            std::string done = preedit();
            reset();
            cho_ = carry;
            jung_ = jung1_ = v;
            return done;
        }
        if (jung_) {
            if (!jung2_) {
                if (uint32_t c = detail::combineVowel(jung1_, v)) { jung_ = c; jung2_ = v; return {}; }
            }
            std::string done = flush();
            jung_ = jung1_ = v;
            return done;
        }
        jung_ = jung1_ = v;   // after a consonant (or alone)
        return {};
    }

    std::string feedConsonant(uint32_t c) {
        if (cho_ && jung_) {
            if (!jong_) {
                if (detail::jongIndex(c)) { jong_ = jong1_ = c; return {}; }
            } else if (!jong2_) {
                if (uint32_t f = detail::combineFinal(jong1_, c)) { jong_ = f; jong2_ = c; return {}; }
            }
        }
        std::string done = flush();
        cho_ = c;
        return done;
    }

    uint32_t cho_ = 0, jung_ = 0, jong_ = 0;
    uint32_t jung1_ = 0, jung2_ = 0;   // parts of a compound vowel
    uint32_t jong1_ = 0, jong2_ = 0;   // parts of a compound final
};

}  // namespace fbx::xserver::hangul
