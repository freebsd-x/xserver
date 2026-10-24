// Pure parser for usbconfig(8) output (shared by the three USB services).

#include "xserver/usb_list.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::usb;

namespace {
// Representative `usbconfig list` output (FreeBSD format).
constexpr const char* kList =
    "ugen0.1: <XHCI root HUB Intel> at usbus0, cfg=0 md=HOST spd=SUPER (5.0Gbps) pwr=SAVE (0mA)\n"
    "ugen0.2: <USB Receiver Logitech> at usbus0, cfg=0 md=HOST spd=LOW (1.5Mbps) pwr=ON (98mA)\n"
    "ugen1.3: <Flash Drive SanDisk> at usbus1, cfg=0 md=HOST spd=HIGH (480Mbps) pwr=ON (200mA)\n";

// Representative `usbconfig -d ugen0.2 dump_device_desc` output.
constexpr const char* kDesc =
    "<USB Receiver Logitech>\n"
    "  bLength = 0x0012 \n"
    "  bDescriptorType = 0x0001 \n"
    "  bcdUSB = 0x0200 \n"
    "  bDeviceClass = 0x0000  <Probed by interface class>\n"
    "  bDeviceSubClass = 0x0000 \n"
    "  bMaxPacketSize0 = 0x0008 \n"
    "  idVendor = 0x046d \n"
    "  idProduct = 0xc52b \n"
    "  bcdDevice = 0x1203 \n"
    "  iManufacturer = 0x0001  <Logitech>\n"
    "  iProduct = 0x0002  <USB Receiver>\n";
}  // namespace

TEST(UsbList, ParsesListLinesBusAddrProduct) {
    auto v = parseList(kList);
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0].bus, 0u);  EXPECT_EQ(v[0].addr, 1u);
    EXPECT_EQ(v[0].product, "XHCI root HUB Intel");
    EXPECT_EQ(v[1].bus, 0u);  EXPECT_EQ(v[1].addr, 2u);
    EXPECT_EQ(v[2].bus, 1u);  EXPECT_EQ(v[2].addr, 3u);
    EXPECT_EQ(v[2].product, "Flash Drive SanDisk");
}

TEST(UsbList, SkipsNonDeviceLines) {
    EXPECT_TRUE(parseList("No USB devices found\n").empty());
    EXPECT_TRUE(parseList("").empty());
}

TEST(UsbList, DumpDescFillsVidPidClassStrings) {
    auto v = parseList(kList);
    ASSERT_EQ(v.size(), 3u);
    parseDeviceDesc(kDesc, v[1]);
    EXPECT_EQ(v[1].vid, 0x046du);
    EXPECT_EQ(v[1].pid, 0xc52bu);
    EXPECT_EQ(v[1].device_class, 0x00);          // class at interface level
    EXPECT_EQ(v[1].manufacturer, "Logitech");
    EXPECT_EQ(v[1].product, "USB Receiver");     // iProduct overrides list text
    EXPECT_EQ(v[1].cls, "HID");                  // derived from product (receiver)
}

TEST(UsbList, ClassStringFromDeviceClassByte) {
    EXPECT_EQ(classString(0x09), "HUB");
    EXPECT_EQ(classString(0x03), "HID");
    EXPECT_EQ(classString(0x08), "MSC");
    EXPECT_EQ(classString(0x01), "AUD");
    EXPECT_EQ(classString(0x02), "CDC");
    EXPECT_EQ(classString(0x00, "SanDisk Flash Drive"), "MSC");  // product hint
    EXPECT_EQ(classString(0x00, "Mystery Gadget"), "other");
}

TEST(UsbList, HubFromProductWhenClassZero) {
    auto v = parseList(kList);
    parseDeviceDesc("  bDeviceClass = 0x0009 \n", v[0]);   // root HUB
    EXPECT_EQ(v[0].cls, "HUB");
}
