// Unit tests for Wake-on-LAN + saved-Wi-Fi pure helpers.

#include "xserver/net_wol.h"
#include "xserver/wifi_saved.h"
#include <gtest/gtest.h>

using namespace fbx::xserver;

// ── WoL: parseMac ─────────────────────────────────────────────────────────────

TEST(Wol, ParseMacColon) {
    uint8_t m[6];
    ASSERT_TRUE(wol::parseMac("AA:BB:CC:DD:EE:FF", m));
    EXPECT_EQ(m[0], 0xAA); EXPECT_EQ(m[5], 0xFF);
}

TEST(Wol, ParseMacDashAndLowercase) {
    uint8_t m[6];
    ASSERT_TRUE(wol::parseMac("01-23-45-67-89-ab", m));
    EXPECT_EQ(m[0], 0x01); EXPECT_EQ(m[3], 0x67); EXPECT_EQ(m[5], 0xAB);
}

TEST(Wol, ParseMacRejectsMalformed) {
    uint8_t m[6];
    EXPECT_FALSE(wol::parseMac("", m));
    EXPECT_FALSE(wol::parseMac("AA:BB:CC:DD:EE", m));        // too short
    EXPECT_FALSE(wol::parseMac("AA:BB:CC:DD:EE:FF:00", m));  // too long
    EXPECT_FALSE(wol::parseMac("GG:BB:CC:DD:EE:FF", m));     // bad hex
    EXPECT_FALSE(wol::parseMac("AABBCCDDEEFF", m));          // no separators
    EXPECT_FALSE(wol::parseMac("AA:BB:CC:DD:EE:FF ", m));    // trailing junk
}

// ── WoL: buildMagicPacket ─────────────────────────────────────────────────────

TEST(Wol, MagicPacketShape) {
    uint8_t m[6] = {0xDE,0xAD,0xBE,0xEF,0x00,0x11};
    auto pkt = wol::buildMagicPacket(m);
    EXPECT_EQ(pkt.size(), 102u);
    for (int i = 0; i < 6; ++i) EXPECT_EQ(pkt[i], 0xFF);   // 6-byte 0xFF header
    // MAC repeated 16×
    for (int r = 0; r < 16; ++r)
        for (int b = 0; b < 6; ++b)
            EXPECT_EQ(pkt[6 + r*6 + b], m[b]) << "rep " << r << " byte " << b;
}

// ── Saved Wi-Fi: parseWpaSupplicant ───────────────────────────────────────────

static const char* kWpa =
    "ctrl_interface=/var/run/wpa_supplicant\n"
    "network={\n"
    "    ssid=\"HomeNet\"\n"
    "    psk=\"secret\"\n"
    "    key_mgmt=WPA-PSK\n"
    "}\n"
    "network={\n"
    "    ssid=\"CafeOpen\"\n"
    "    key_mgmt=NONE\n"
    "    disabled=1\n"
    "}\n"
    "network={\n"
    "    ssid=\"Mesh6E\"\n"
    "    key_mgmt=SAE\n"
    "}\n";

TEST(WifiSaved, ParsesAllBlocks) {
    auto v = wifi::parseWpaSupplicant(kWpa);
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0].ssid, "HomeNet");
    EXPECT_EQ(v[1].ssid, "CafeOpen");
    EXPECT_EQ(v[2].ssid, "Mesh6E");
}

TEST(WifiSaved, SecurityClassification) {
    auto v = wifi::parseWpaSupplicant(kWpa);
    EXPECT_EQ(v[0].security, 1u);  // WPA-PSK → WPA2
    EXPECT_EQ(v[1].security, 0u);  // NONE → open
    EXPECT_EQ(v[2].security, 2u);  // SAE → WPA3
}

TEST(WifiSaved, DisabledMeansNoAutoConnect) {
    auto v = wifi::parseWpaSupplicant(kWpa);
    EXPECT_EQ(v[0].auto_connect, 1u);
    EXPECT_EQ(v[1].auto_connect, 0u);  // disabled=1
    EXPECT_EQ(v[2].auto_connect, 1u);
}

TEST(WifiSaved, EmptyAndNoSsidSkipped) {
    EXPECT_TRUE(wifi::parseWpaSupplicant("").empty());
    EXPECT_TRUE(wifi::parseWpaSupplicant("network={\n key_mgmt=NONE\n}\n").empty());
}

TEST(WifiSaved, SecurityHelperDirect) {
    EXPECT_EQ(wifi::securityFromKeyMgmt("SAE"), 2u);
    EXPECT_EQ(wifi::securityFromKeyMgmt("NONE"), 0u);
    EXPECT_EQ(wifi::securityFromKeyMgmt("WPA-EAP"), 1u);
}
