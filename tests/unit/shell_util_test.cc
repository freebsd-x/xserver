// Tests for shared/shell_util.h — shellQuote and isValidXxx predicates.
// Pure header-only logic; no I/O, no sockets, no Vulkan.

#include "xserver/shell_util.h"
#include <gtest/gtest.h>

namespace fbx::xserver::testing {

// ── shellQuote ────────────────────────────────────────────────────────────────

TEST(ShellQuoteTest, PlainString) {
    EXPECT_EQ(shellQuote("hello"), "'hello'");
}

TEST(ShellQuoteTest, EmptyString) {
    EXPECT_EQ(shellQuote(""), "''");
}

TEST(ShellQuoteTest, StringWithSpaces) {
    EXPECT_EQ(shellQuote("hello world"), "'hello world'");
}

TEST(ShellQuoteTest, StringWithDollarSign) {
    EXPECT_EQ(shellQuote("$HOME"), "'$HOME'");
}

TEST(ShellQuoteTest, StringWithBacktick) {
    EXPECT_EQ(shellQuote("`rm -rf /`"), "'`rm -rf /`'");
}

TEST(ShellQuoteTest, StringWithSingleQuote) {
    // "it's" → 'it'\''s'
    EXPECT_EQ(shellQuote("it's"), "'it'\\''s'");
}

TEST(ShellQuoteTest, StringWithSemicolon) {
    EXPECT_EQ(shellQuote("a;b"), "'a;b'");
}

TEST(ShellQuoteTest, StringWithAmpersand) {
    EXPECT_EQ(shellQuote("a&&b"), "'a&&b'");
}

TEST(ShellQuoteTest, StringWithPipe) {
    EXPECT_EQ(shellQuote("a|b"), "'a|b'");
}

TEST(ShellQuoteTest, MultipleEmbeddedSingleQuotes) {
    // "a'b'c" → 'a'\''b'\''c'
    EXPECT_EQ(shellQuote("a'b'c"), "'a'\\''b'\\''c'");
}

TEST(ShellQuoteTest, OnlySingleQuote) {
    EXPECT_EQ(shellQuote("'"), "''\\'''");
}

// ── isValidMacAddress ─────────────────────────────────────────────────────────

TEST(MacAddressTest, CanonicalForm) {
    EXPECT_TRUE(isValidMacAddress("00:11:22:33:44:55"));
    EXPECT_TRUE(isValidMacAddress("AA:BB:CC:DD:EE:FF"));
    EXPECT_TRUE(isValidMacAddress("a1:b2:c3:d4:e5:f6"));
}

TEST(MacAddressTest, TooShort) {
    EXPECT_FALSE(isValidMacAddress("00:11:22:33:44"));
}

TEST(MacAddressTest, TooLong) {
    EXPECT_FALSE(isValidMacAddress("00:11:22:33:44:55:66"));
}

TEST(MacAddressTest, WrongSeparator) {
    EXPECT_FALSE(isValidMacAddress("00-11-22-33-44-55"));
}

TEST(MacAddressTest, ShellInjectionAttempt) {
    EXPECT_FALSE(isValidMacAddress("00:11:22:33:44:5;"));
    EXPECT_FALSE(isValidMacAddress("$(cmd):22:33:44:55"));
}

TEST(MacAddressTest, Empty) {
    EXPECT_FALSE(isValidMacAddress(""));
}

// ── isValidInterfaceName ──────────────────────────────────────────────────────

TEST(InterfaceNameTest, Typical) {
    EXPECT_TRUE(isValidInterfaceName("wlan0"));
    EXPECT_TRUE(isValidInterfaceName("em0"));
    EXPECT_TRUE(isValidInterfaceName("wg0"));
    EXPECT_TRUE(isValidInterfaceName("vtnet0"));
}

TEST(InterfaceNameTest, WithHyphenAndUnderscore) {
    EXPECT_TRUE(isValidInterfaceName("my-iface"));
    EXPECT_TRUE(isValidInterfaceName("my_iface"));
}

TEST(InterfaceNameTest, MaxLength16) {
    EXPECT_TRUE(isValidInterfaceName("abcdefghijklmnop"));   // 16 chars
    EXPECT_FALSE(isValidInterfaceName("abcdefghijklmnopq")); // 17 chars
}

TEST(InterfaceNameTest, Empty) {
    EXPECT_FALSE(isValidInterfaceName(""));
}

TEST(InterfaceNameTest, ShellInjection) {
    EXPECT_FALSE(isValidInterfaceName("wlan0;rm -rf /"));
    EXPECT_FALSE(isValidInterfaceName("wlan0|cat /etc/passwd"));
    EXPECT_FALSE(isValidInterfaceName("wlan0$HOME"));
}

// ── isValidZfsName ────────────────────────────────────────────────────────────

TEST(ZfsNameTest, SimpleDataset) {
    EXPECT_TRUE(isValidZfsName("tank/data"));
    EXPECT_TRUE(isValidZfsName("zroot/usr/home"));
}

TEST(ZfsNameTest, ColonAndHyphenAllowed) {
    // ZFS clone names use colon; @ is NOT in the whitelist (treated as shell-unsafe).
    EXPECT_TRUE(isValidZfsName("tank:clone"));
    EXPECT_TRUE(isValidZfsName("tank/data-backup"));
    EXPECT_FALSE(isValidZfsName("tank/data@snap")); // @ not in whitelist
}

TEST(ZfsNameTest, WithDot) {
    EXPECT_TRUE(isValidZfsName("tank/data.bak"));
}

TEST(ZfsNameTest, Empty) {
    EXPECT_FALSE(isValidZfsName(""));
}

TEST(ZfsNameTest, TooLong) {
    std::string s(257, 'a');
    EXPECT_FALSE(isValidZfsName(s));
}

TEST(ZfsNameTest, ShellInjection) {
    EXPECT_FALSE(isValidZfsName("tank;rm -rf /"));
    EXPECT_FALSE(isValidZfsName("tank`id`"));
    EXPECT_FALSE(isValidZfsName("tank$(evil)"));
}

// ── isValidPrinterName ────────────────────────────────────────────────────────

TEST(PrinterNameTest, Typical) {
    EXPECT_TRUE(isValidPrinterName("HP_LaserJet"));
    EXPECT_TRUE(isValidPrinterName("Brother-HL-L2350DW"));
    EXPECT_TRUE(isValidPrinterName("PDF.printer"));
}

TEST(PrinterNameTest, MaxLength64) {
    std::string s(64, 'a');
    EXPECT_TRUE(isValidPrinterName(s));
    s += 'a';
    EXPECT_FALSE(isValidPrinterName(s));
}

TEST(PrinterNameTest, Empty) {
    EXPECT_FALSE(isValidPrinterName(""));
}

TEST(PrinterNameTest, ShellInjection) {
    EXPECT_FALSE(isValidPrinterName("printer;rm -rf /"));
    EXPECT_FALSE(isValidPrinterName("printer|cat /etc/passwd"));
    EXPECT_FALSE(isValidPrinterName("printer$(evil)"));
}

} // namespace fbx::xserver::testing
