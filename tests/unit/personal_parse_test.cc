// TOTP (RFC 6238 vectors), text replacements, reminders, license parsing.

#include "xserver/totp.h"
#include "xserver/personal_parse.h"
#include "shared/text_replace.h"
#include <gtest/gtest.h>

using namespace fbx::xserver;
namespace textreplace = fbx::shared::textreplace;

// ── TOTP ─────────────────────────────────────────────────────────────────────

namespace {
totp::Account rfcAccount(const char* algo, std::string_view seed) {
    totp::Account a;
    a.algorithm = algo;
    a.digits = 8;
    a.secret.assign(seed.begin(), seed.end());
    return a;
}
}  // namespace

TEST(Totp, Rfc6238AppendixBVectors) {
    auto sha1   = rfcAccount("SHA1",   "12345678901234567890");
    auto sha256 = rfcAccount("SHA256", "12345678901234567890123456789012");
    auto sha512 = rfcAccount("SHA512", "1234567890123456789012345678901234567890123456789012345678901234");
    EXPECT_EQ(totp::code(sha1, 59), "94287082");
    EXPECT_EQ(totp::code(sha1, 1111111109), "07081804");     // leading zero kept
    EXPECT_EQ(totp::code(sha1, 1234567890), "89005924");
    EXPECT_EQ(totp::code(sha1, 20000000000), "65353130");
    EXPECT_EQ(totp::code(sha256, 59), "46119246");
    EXPECT_EQ(totp::code(sha256, 1111111109), "68084774");
    EXPECT_EQ(totp::code(sha512, 59), "90693936");
    EXPECT_EQ(totp::code(sha512, 1111111109), "25091201");
    EXPECT_EQ(totp::secondsRemaining(sha1, 59), 1u);
    EXPECT_EQ(totp::secondsRemaining(sha1, 60), 30u);
}

TEST(Totp, Base32AndUriParsing) {
    std::vector<uint8_t> out;
    ASSERT_TRUE(totp::base32Decode("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ", out));
    EXPECT_EQ(std::string(out.begin(), out.end()), "12345678901234567890");
    ASSERT_TRUE(totp::base32Decode("gezd gnbv====", out));        // case/space/padding
    EXPECT_FALSE(totp::base32Decode("GEZ1", out));                  // '1' isn't base32

    totp::Account a;
    ASSERT_TRUE(totp::parseUri(
        "otpauth://totp/GitHub:octo%40example.com?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&issuer=GitHub", a));
    EXPECT_EQ(a.issuer, "GitHub");
    EXPECT_EQ(a.account, "octo@example.com");
    EXPECT_EQ(a.digits, 6u);
    EXPECT_EQ(a.period, 30u);
    EXPECT_EQ(totp::code(a, 59), "287082");                          // 6-digit tail of 94287082

    totp::Account b;
    ASSERT_TRUE(totp::parseUri("otpauth://totp/me?secret=GEZDGNBV&algorithm=sha256&digits=8&period=60", b));
    EXPECT_EQ(b.algorithm, "SHA256");
    EXPECT_EQ(b.digits, 8u);
    EXPECT_EQ(b.period, 60u);
    EXPECT_EQ(b.issuer, "");

    totp::Account c;
    EXPECT_FALSE(totp::parseUri("otpauth://hotp/x?secret=GEZDGNBV&counter=1", c));   // needs a counter
    EXPECT_FALSE(totp::parseUri("otpauth://totp/x?issuer=nosecret", c));
    EXPECT_FALSE(totp::parseUri("otpauth://totp/x?secret=GEZDGNBV&digits=4", c));
    EXPECT_FALSE(totp::parseUri("otpauth://totp/x?secret=GEZDGNBV&algorithm=MD5", c));

    auto all = totp::parseFile("# mine\notpauth://totp/A:a?secret=GEZDGNBV\n\njunk\n"
                               "otpauth://totp/B:b?secret=GEZDGNBV\r\n");
    ASSERT_EQ(all.size(), 2u);
    EXPECT_NE(totp::accountId(all[0]), totp::accountId(all[1]));
    EXPECT_EQ(totp::accountId(all[0]).size(), 8u);
}

// ── Text replacement ─────────────────────────────────────────────────────────

TEST(TextReplace, ParseSerializeAndValidate) {
    auto e = textreplace::parse("# header\nomw\tOn my way!\nbad line\n\tno shortcut\nbrb\tBe right back\nomw\tOn my way\n");
    ASSERT_EQ(e.size(), 2u);
    EXPECT_EQ(e[0].shortcut, "omw");
    EXPECT_EQ(e[0].phrase, "On my way");                 // last one wins
    EXPECT_EQ(textreplace::parse(textreplace::serialize(e)).size(), 2u);
    EXPECT_FALSE(textreplace::validShortcut("two words"));
    EXPECT_FALSE(textreplace::validShortcut(""));
    EXPECT_FALSE(textreplace::validPhrase("line\nbreak"));
    EXPECT_NE(textreplace::entryId("omw"), textreplace::entryId("brb"));
}

TEST(TextReplace, MatchesWholeWordBeforeCursor) {
    std::vector<textreplace::Entry> e{{"omw", "On my way"}, {"@@", "me@example.com"}};
    size_t b = 0;
    const std::string* p = textreplace::match(e, "ok omw", 6, b);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, "On my way");
    EXPECT_EQ(b, 3u);
    EXPECT_EQ(textreplace::match(e, "nomw", 4, b), nullptr);       // part of a longer word
    EXPECT_EQ(textreplace::match(e, "OMW", 3, b), nullptr);        // case-sensitive
    ASSERT_NE(textreplace::match(e, "mail @@", 7, b), nullptr);
    EXPECT_EQ(textreplace::match(e, "", 0, b), nullptr);
    EXPECT_TRUE(textreplace::isBoundary(' '));
    EXPECT_TRUE(textreplace::isBoundary('.'));
    EXPECT_FALSE(textreplace::isBoundary('a'));
}

// ── Reminders ────────────────────────────────────────────────────────────────

TEST(Reminders, WindowDoneAndOrder) {
    const std::time_t now = 1'800'000'000;
    std::vector<personal::Reminder> all{
        {1, "later this week", now + 3 * 86400, now + 3 * 86400 - 600},
        {2, "overdue",         now - 3600,      now - 3600 - 900},
        {3, "too old",         now - 3 * 86400, now - 3 * 86400},
        {4, "next month",      now + 30 * 86400, now + 30 * 86400},
        {5, "done",            now + 3600,      now + 3000},
        {6, "soon",            now + 600,       now + 300},
    };
    auto r = personal::selectReminders(all, now, {5});
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0].event_id, 2u);      // overdue first (earliest due)
    EXPECT_EQ(r[1].event_id, 6u);
    EXPECT_EQ(r[2].event_id, 1u);
}

TEST(Reminders, WireIdsAndDoneSet) {
    uint32_t id = 0;
    EXPECT_EQ(personal::reminderWireId(42), "ev-42");
    ASSERT_TRUE(personal::parseReminderWireId("ev-42", id));
    EXPECT_EQ(id, 42u);
    EXPECT_FALSE(personal::parseReminderWireId("ev-", id));
    EXPECT_FALSE(personal::parseReminderWireId("ev-0", id));
    EXPECT_FALSE(personal::parseReminderWireId("ev-12a", id));
    EXPECT_FALSE(personal::parseReminderWireId("ev-99999999999", id));
    auto set = personal::parseIdSet("3;1;x;;7");
    EXPECT_EQ(set, (std::set<uint32_t>{1, 3, 7}));
    EXPECT_EQ(personal::formatIdSet(set), "1;3;7");
}

// ── License ──────────────────────────────────────────────────────────────────

TEST(License, KeyValuesAndMask) {
    auto kv = personal::parseKeyValues("# installer\n key = ABCD-EFGH-IJKL-A3F7 \ntier=Pro\nnoequals\n");
    EXPECT_EQ(kv["key"], "ABCD-EFGH-IJKL-A3F7");
    EXPECT_EQ(kv["tier"], "Pro");
    EXPECT_EQ(kv.count("noequals"), 0u);
    std::string m = personal::maskKey(kv["key"]);
    EXPECT_EQ(m, "****-****-****-A3F7");
    EXPECT_LT(m.size(), 32u);
    EXPECT_EQ(m.find("ABCD"), std::string::npos);
    EXPECT_EQ(personal::maskKey("1234"), "****-****-****-");          // too short: show nothing
    EXPECT_EQ(personal::maskKey(""), "");
}
