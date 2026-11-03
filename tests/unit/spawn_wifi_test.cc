// Shell-free process spawning (async_shell.h spawnArgvCapture) and the Wi-Fi
// join commands (wifi_commands.h, wpa_ctrl.h) — the compositor no longer puts
// client data into shell strings or secrets on command lines.

#include "xserver/async_shell.h"
#include "xserver/wifi_commands.h"
#include "xserver/wpa_ctrl.h"

#include <gtest/gtest.h>

#include <chrono>

using namespace fbx::xserver;

TEST(SpawnArgv, ArgumentsAreNeverShellSyntax) {
    const std::string nasty = "$(id); `whoami` | rm -rf / & '\" > /tmp/x";
    EXPECT_EQ(spawnArgvCapture({"printf", "%s", nasty}), nasty);
}

TEST(SpawnArgv, StdinDataReachesTheProgram) {
    EXPECT_EQ(spawnArgvCapture({"cat"}, {.stdin_data = "X-API-Key: secret\n"}), "X-API-Key: secret\n");
}

TEST(SpawnArgv, ExitCodeAndTimeout) {
    int code = 0;
    (void)spawnArgvCapture({"false"}, {.exit_code = &code});
    EXPECT_EQ(code, 1);
    EXPECT_EQ(spawnArgvRun({"true"}), 0);
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_NE(spawnArgvRun({"sleep", "10"}, 1), 0);   // timeout(1) stops it
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(5));
    EXPECT_EQ(spawnArgvRun({"/nonexistent/program"}), -1);
}

// IEEE 802.11i-2004, Annex H.4.1 — the PSK test vectors.
TEST(Wifi, PskMatchesTheStandardTestVectors) {
    EXPECT_EQ(wifi::pskHex("password", "IEEE"),
              "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
    EXPECT_EQ(wifi::pskHex("ThisIsAPassword", "ThisIsASSID"),
              "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af");
}

TEST(Wifi, InvalidPassphrasesAreRefused) {
    EXPECT_EQ(wifi::pskHex("short", "Net"), "");                    // < 8
    EXPECT_EQ(wifi::pskHex(std::string(64, 'a'), "Net"), "");       // > 63
    EXPECT_EQ(wifi::pskHex("tab\tinside", "Net"), "");              // not printable ASCII
    EXPECT_TRUE(wifi::joinCommands("3", "Net", "short").empty());
}

TEST(Wifi, JoinCommandsUseTheNewNetworkAndHexSsid) {
    // An SSID with quotes and spaces — the kind the old quoting got wrong.
    auto cmds = wifi::joinCommands("7", "My \"Home\" Wi-Fi", "password1234");
    ASSERT_EQ(cmds.size(), 4u);
    EXPECT_EQ(cmds[0], "SET_NETWORK 7 ssid " + wifi::ssidHex("My \"Home\" Wi-Fi"));
    EXPECT_EQ(wifi::ssidHex("IEEE"), "49454545");
    EXPECT_EQ(cmds[1].rfind("SET_NETWORK 7 psk ", 0), 0u);
    EXPECT_EQ(cmds[1].size(), std::string("SET_NETWORK 7 psk ").size() + 64);   // derived key, not the passphrase
    EXPECT_EQ(cmds[1].find("password1234"), std::string::npos);
    EXPECT_EQ(cmds[2], "ENABLE_NETWORK 7");
    EXPECT_EQ(cmds[3], "SELECT_NETWORK 7");

    auto open = wifi::joinCommands("0", "Cafe", "");
    ASSERT_EQ(open.size(), 4u);
    EXPECT_EQ(open[1], "SET_NETWORK 0 key_mgmt NONE");
}

TEST(Wifi, NetworkIdsAndInterfacesAreValidated) {
    EXPECT_TRUE(wifi::validNetworkId("0"));
    EXPECT_TRUE(wifi::validNetworkId("12"));
    EXPECT_FALSE(wifi::validNetworkId("FAIL"));
    EXPECT_FALSE(wifi::validNetworkId(""));
    EXPECT_FALSE(wifi::validNetworkId("1 2"));
    EXPECT_TRUE(wpa::validIface("wlan0"));
    EXPECT_FALSE(wpa::validIface("../../etc/passwd"));
    EXPECT_FALSE(wpa::validIface("wlan0/x"));
    EXPECT_EQ(wpa::request("../evil", "STATUS"), "");   // refused before any socket is opened
}
