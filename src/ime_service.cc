#include "xserver/ime_service.h"
#include "xserver/ipc_socket.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

namespace fbx::xserver {

// ─── Static state ─────────────────────────────────────────────────────────────

std::shared_ptr<IpcSocket> ImeService::active_client_;
uint32_t                   ImeService::locale_      = 0;
std::string                ImeService::composition_;

// ─── UTF-8 helpers ────────────────────────────────────────────────────────────

static std::string encodeUtf8(uint32_t cp) noexcept {
    std::string s;
    if (cp < 0x80u) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800u) {
        s += static_cast<char>(0xC0u | (cp >> 6));
        s += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else {
        s += static_cast<char>(0xE0u | (cp >> 12));
        s += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        s += static_cast<char>(0x80u | (cp & 0x3Fu));
    }
    return s;
}

// ─── Romaji → Hiragana table ──────────────────────────────────────────────────
// Each entry maps a lowercase ASCII romaji string to a pre-encoded UTF-8
// hiragana string (monograph or digraph).  Longer entries must appear before
// shorter ones so that the prefix-matching logic terminates correctly.

struct REntry { std::string romaji; std::string utf8; };

static std::vector<REntry> g_table; // built lazily on first use

static void ensureTable() noexcept {
    if (!g_table.empty()) return;

    // Helper lambdas
    auto H  = [](uint32_t cp)              { return encodeUtf8(cp); };
    auto H2 = [](uint32_t a, uint32_t b)   { return encodeUtf8(a) + encodeUtf8(b); };

    // Codepoints used:
    // Vowels: あ=3042 い=3044 う=3046 え=3048 お=304A
    // K: か=304B き=304D く=304F け=3051 こ=3053
    // G: が=304C ぎ=304E ぐ=3050 げ=3052 ご=3054
    // S/Z: さ=3055 し=3057 す=3059 せ=305B そ=305D
    //      ざ=3056 じ=3058 ず=305A ぜ=305C ぞ=305E
    // T/D: た=305F ち=3061 つ=3064 て=3066 と=3068
    //      だ=3060 ぢ=3062 づ=3065 で=3067 ど=3069
    // N:   な=306A に=306B ぬ=306C ね=306D の=306E
    // H/B/P: は=306F ひ=3072 ふ=3075 へ=3078 ほ=307B
    //        ば=3070 び=3073 ぶ=3076 べ=3079 ぼ=307C
    //        ぱ=3071 ぴ=3074 ぷ=3077 ぺ=307A ぽ=307D
    // M:   ま=307E み=307F む=3080 め=3081 も=3082
    // Y:   や=3084 ゆ=3086 よ=3088
    // R:   ら=3089 り=308A る=308B れ=308C ろ=308D
    // W/N: わ=308F を=3092 ん=3093
    // Small: っ=3063 ゃ=3083 ゅ=3085 ょ=3087
    //        ぁ=3041 ぃ=3043 ぅ=3045 ぇ=3047 ぉ=3049

    g_table = {
        // ── Digraphs (must be before shorter prefixes) ─────────────────────
        // S/Z digraphs
        {"sha",  H2(0x3057,0x3083)}, // しゃ
        {"shi",  H(0x3057)},         // し
        {"shu",  H2(0x3057,0x3085)}, // しゅ
        {"sho",  H2(0x3057,0x3087)}, // しょ
        {"ja",   H2(0x3058,0x3083)}, // じゃ
        {"ji",   H(0x3058)},         // じ
        {"ju",   H2(0x3058,0x3085)}, // じゅ
        {"jo",   H2(0x3058,0x3087)}, // じょ
        // T/D digraphs
        {"cha",  H2(0x3061,0x3083)}, // ちゃ
        {"chi",  H(0x3061)},         // ち
        {"chu",  H2(0x3061,0x3085)}, // ちゅ
        {"cho",  H2(0x3061,0x3087)}, // ちょ
        {"tsu",  H(0x3064)},         // つ
        // K digraphs
        {"kya",  H2(0x304D,0x3083)}, // きゃ
        {"kyu",  H2(0x304D,0x3085)}, // きゅ
        {"kyo",  H2(0x304D,0x3087)}, // きょ
        // G digraphs
        {"gya",  H2(0x304E,0x3083)}, // ぎゃ
        {"gyu",  H2(0x304E,0x3085)}, // ぎゅ
        {"gyo",  H2(0x304E,0x3087)}, // ぎょ
        // N digraphs
        {"nya",  H2(0x306B,0x3083)}, // にゃ
        {"nyu",  H2(0x306B,0x3085)}, // にゅ
        {"nyo",  H2(0x306B,0x3087)}, // にょ
        // H digraphs
        {"hya",  H2(0x3072,0x3083)}, // ひゃ
        {"hyu",  H2(0x3072,0x3085)}, // ひゅ
        {"hyo",  H2(0x3072,0x3087)}, // ひょ
        // B digraphs
        {"bya",  H2(0x3073,0x3083)}, // びゃ
        {"byu",  H2(0x3073,0x3085)}, // びゅ
        {"byo",  H2(0x3073,0x3087)}, // びょ
        // P digraphs
        {"pya",  H2(0x3074,0x3083)}, // ぴゃ
        {"pyu",  H2(0x3074,0x3085)}, // ぴゅ
        {"pyo",  H2(0x3074,0x3087)}, // ぴょ
        // M digraphs
        {"mya",  H2(0x307F,0x3083)}, // みゃ
        {"myu",  H2(0x307F,0x3085)}, // みゅ
        {"myo",  H2(0x307F,0x3087)}, // みょ
        // R digraphs
        {"rya",  H2(0x308A,0x3083)}, // りゃ
        {"ryu",  H2(0x308A,0x3085)}, // りゅ
        {"ryo",  H2(0x308A,0x3087)}, // りょ
        // Small kana (x prefix — must come before single-letter entries)
        {"xtu",  H(0x3063)},         // っ
        {"xya",  H(0x3083)},         // ゃ
        {"xyu",  H(0x3085)},         // ゅ
        {"xyo",  H(0x3087)},         // ょ
        {"xa",   H(0x3041)},         // ぁ
        {"xi",   H(0x3043)},         // ぃ
        {"xu",   H(0x3045)},         // ぅ
        {"xe",   H(0x3047)},         // ぇ
        {"xo",   H(0x3049)},         // ぉ
        // ── Monographs ───────────────────────────────────────────────────────
        // Vowels
        {"a",    H(0x3042)},  // あ
        {"i",    H(0x3044)},  // い
        {"u",    H(0x3046)},  // う
        {"e",    H(0x3048)},  // え
        {"o",    H(0x304A)},  // お
        // K row
        {"ka",   H(0x304B)}, {"ki",   H(0x304D)}, {"ku",   H(0x304F)},
        {"ke",   H(0x3051)}, {"ko",   H(0x3053)},
        // G row
        {"ga",   H(0x304C)}, {"gi",   H(0x304E)}, {"gu",   H(0x3050)},
        {"ge",   H(0x3052)}, {"go",   H(0x3054)},
        // S row (shi handled above)
        {"sa",   H(0x3055)}, {"si",   H(0x3057)}, {"su",   H(0x3059)},
        {"se",   H(0x305B)}, {"so",   H(0x305D)},
        // Z row
        {"za",   H(0x3056)}, {"zi",   H(0x3058)}, {"zu",   H(0x305A)},
        {"ze",   H(0x305C)}, {"zo",   H(0x305E)},
        // T row (chi/tsu handled above)
        {"ta",   H(0x305F)}, {"ti",   H(0x3061)}, {"tu",   H(0x3064)},
        {"te",   H(0x3066)}, {"to",   H(0x3068)},
        // D row
        {"da",   H(0x3060)}, {"di",   H(0x3062)}, {"du",   H(0x3065)},
        {"de",   H(0x3067)}, {"do",   H(0x3069)},
        // N row
        {"na",   H(0x306A)}, {"ni",   H(0x306B)}, {"nu",   H(0x306C)},
        {"ne",   H(0x306D)}, {"no",   H(0x306E)},
        // H row
        {"ha",   H(0x306F)}, {"hi",   H(0x3072)}, {"fu",   H(0x3075)},
        {"hu",   H(0x3075)}, {"he",   H(0x3078)}, {"ho",   H(0x307B)},
        // B row
        {"ba",   H(0x3070)}, {"bi",   H(0x3073)}, {"bu",   H(0x3076)},
        {"be",   H(0x3079)}, {"bo",   H(0x307C)},
        // P row
        {"pa",   H(0x3071)}, {"pi",   H(0x3074)}, {"pu",   H(0x3077)},
        {"pe",   H(0x307A)}, {"po",   H(0x307D)},
        // M row
        {"ma",   H(0x307E)}, {"mi",   H(0x307F)}, {"mu",   H(0x3080)},
        {"me",   H(0x3081)}, {"mo",   H(0x3082)},
        // Y row
        {"ya",   H(0x3084)}, {"yu",   H(0x3086)}, {"yo",   H(0x3088)},
        // R row
        {"ra",   H(0x3089)}, {"ri",   H(0x308A)}, {"ru",   H(0x308B)},
        {"re",   H(0x308C)}, {"ro",   H(0x308D)},
        // W row
        {"wa",   H(0x308F)}, {"wi",   H(0x3090)}, {"we",   H(0x3091)},
        {"wo",   H(0x3092)},
        // N (double-n for ん; single-n is handled as a special case in FilterKeyPress)
        {"nn",   H(0x3093)},  // ん
    };
}

// ─── Internal helpers ─────────────────────────────────────────────────────────

shared::InputMessage ImeService::makeTextMsg(shared::InputEventType type,
                                             const std::string& utf8) noexcept {
    shared::InputMessage msg{};
    msg.type_ = type;
    uint32_t len = static_cast<uint32_t>(std::min(utf8.size(), size_t{8}));
    msg.modifiers_ = len;
    if (len > 0)
        std::memcpy(&msg.value_a_, utf8.data(), std::min(len, 4u));
    if (len > 4)
        std::memcpy(&msg.value_b_, utf8.data() + 4, len - 4u);
    return msg;
}

void ImeService::sendPreedit(const std::string& preedit) noexcept {
    if (!active_client_) return;
    auto m = makeTextMsg(shared::InputEventType::ImePreedit, preedit);
    (void)active_client_->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

void ImeService::sendCommit(const std::string& utf8) noexcept {
    if (!active_client_ || utf8.empty()) return;
    auto m = makeTextMsg(shared::InputEventType::ImeCommit, utf8);
    (void)active_client_->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

void ImeService::clearComposition() noexcept {
    composition_.clear();
    sendPreedit(""); // clear preedit display
}

bool ImeService::tryCommitRomaji() noexcept {
    ensureTable();

    // Exact match → commit and clear
    for (const auto& e : g_table) {
        if (e.romaji == composition_) {
            sendCommit(e.utf8);
            composition_.clear();
            sendPreedit(""); // ensure preedit is cleared
            return true;
        }
    }

    // Prefix match → keep preedit visible, wait for more input
    for (const auto& e : g_table) {
        if (e.romaji.starts_with(composition_)) {
            sendPreedit(composition_);
            return true;
        }
    }

    // No match: try to salvage the prefix by committing it, then retry with tail
    if (composition_.size() > 1) {
        std::string prefix = composition_.substr(0, composition_.size() - 1);
        char tail = composition_.back();

        bool committed = false;
        for (const auto& e : g_table) {
            if (e.romaji == prefix) {
                sendCommit(e.utf8);
                committed = true;
                break;
            }
        }
        composition_.clear();
        if (!committed) {
            // Prefix itself is invalid; forward tail as plain ASCII
            sendPreedit("");
            return false;
        }
        composition_ += tail;
        return tryCommitRomaji(); // recurse with remaining char
    }

    // Single unrecognised character — pass through
    sendPreedit("");
    composition_.clear();
    return false;
}

// ─── FilterKeyPress ───────────────────────────────────────────────────────────

bool ImeService::FilterKeyPress(const std::shared_ptr<IpcSocket>& focused_client,
                                const shared::InputMessage& msg) noexcept {
    // If the focused client changed, silently deactivate IME
    if (active_client_ != focused_client) {
        composition_.clear();
        locale_ = 0;
        active_client_ = nullptr;
        return false;
    }

    if (locale_ == 0) return false; // passthrough — no interception

    // Only handle key-down events
    if (msg.value_b_ == 0) return false; // key release

    // Never intercept modified keys (Ctrl, Alt, Super)
    if (msg.modifiers_ & 0xEu) return false; // bits 1(Ctrl) 2(Alt) 3(Super)

    uint32_t key = static_cast<uint32_t>(msg.value_a_);

    // ── Backspace ─────────────────────────────────────────────────────────────
    if (key == 14) {
        if (!composition_.empty()) {
            composition_.pop_back();
            sendPreedit(composition_);
            return true; // consumed
        }
        return false; // let app handle normal delete
    }

    // ── Escape: cancel composition ────────────────────────────────────────────
    if (key == 1) {
        if (!composition_.empty()) {
            clearComposition();
            return true;
        }
        return false;
    }

    // ── Enter: commit raw romaji as ASCII, then pass Enter through ────────────
    if (key == 28) {
        if (!composition_.empty()) {
            // Commit each ASCII character individually as a regular KeyPress
            for (char c : composition_) {
                shared::InputMessage ascii_msg{};
                ascii_msg.type_       = shared::InputEventType::ImeCommit;
                ascii_msg.char_value_ = static_cast<int32_t>(c);
                ascii_msg.modifiers_  = 1;
                std::memcpy(&ascii_msg.value_a_, &c, 1);
                (void)active_client_->sendMessage(
                    {reinterpret_cast<const uint8_t*>(&ascii_msg), sizeof(ascii_msg)});
            }
            clearComposition();
        }
        return false; // let Enter through for newline / form submission
    }

    // ── Only intercept printable ASCII a-z (and x for small kana) ────────────
    if (msg.char_value_ == 0) {
        // Non-printable (arrows, F-keys, etc.) — commit pending if any, pass through
        if (!composition_.empty()) {
            sendCommit(composition_); // commit raw
            clearComposition();
        }
        return false;
    }

    char ch = static_cast<char>(msg.char_value_);
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) {
        // Digit, punctuation, space etc. — commit pending and pass through
        if (!composition_.empty()) {
            sendCommit(composition_); // raw romaji
            clearComposition();
        }
        return false;
    }

    // Normalise to lowercase
    char lc = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;

    // ── Special "n" rule ──────────────────────────────────────────────────────
    // If composition is "n" and the next char is a consonant (not n, vowel),
    // commit ん first then start a fresh composition with the new char.
    if (composition_ == "n" &&
        lc != 'n' && lc != 'a' && lc != 'i' && lc != 'u' && lc != 'e' && lc != 'o') {
        sendCommit(encodeUtf8(0x3093)); // ん
        composition_.clear();
        // Fall through: append lc below
    }

    // ── Double-consonant → っ ─────────────────────────────────────────────────
    // e.g. "kk" → っ + restart with "k"
    if (composition_.size() == 1 && composition_[0] == lc &&
        lc != 'n' && lc != 'a' && lc != 'i' && lc != 'u' && lc != 'e' && lc != 'o') {
        sendCommit(encodeUtf8(0x3063)); // っ
        composition_.clear();
        // Fall through: append lc below
    }

    composition_ += lc;
    return tryCommitRomaji();
}

// ─── XBus handler ─────────────────────────────────────────────────────────────

void ImeService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                const shared::XBusMessage& msg) noexcept {
    auto op = static_cast<shared::IMEOpcode>(msg.opcode_);

    if (op == shared::IMEOpcode::Activate) {
        const auto* req = reinterpret_cast<const shared::IMEActivateRequest*>(msg.payload_);
        // Flush any pending composition from the previous client
        if (active_client_ && active_client_ != client && !composition_.empty()) {
            clearComposition();
        }
        active_client_ = client;
        locale_        = req->locale_;
        composition_.clear();
        std::cout << "[ime] Activated for client, locale=" << locale_ << "\n";
    }
    else if (op == shared::IMEOpcode::Deactivate) {
        if (active_client_ == client) {
            if (!composition_.empty()) clearComposition();
            active_client_ = nullptr;
            locale_        = 0;
            std::cout << "[ime] Deactivated\n";
        }
    }
}

} // namespace fbx::xserver
