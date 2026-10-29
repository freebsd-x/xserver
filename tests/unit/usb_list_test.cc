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

// ── Unprivileged devinfo path + capacity by serial ───────────────────────────

#include "xserver/geom_parse.h"

TEST(UsbDevinfo, ListsAttachedDevicesOncePerUgen) {
    auto devs = fbx::xserver::usb::parseDevinfo(
        "nexus0\n"
        "      usbus0\n"
        "        uhub0\n"
        "          umass0 pnpinfo vendor=0x0781 product=0x5567 devclass=0x00 devsubclass=0x00 devproto=0x00 "
        "sernum=\"4C530001231218115283\" release=0x0126 mode=host intclass=0x08 intsubclass=0x06 intprotocol=0x50 "
        "at bus=0 hubaddr=1 port=2 devaddr=3 interface=0 ugen=ugen0.3\n"
        "          ukbd0 pnpinfo vendor=0x046d product=0xc52b devclass=0x00 devsubclass=0x00 devproto=0x00 "
        "sernum=\"\" release=0x1211 mode=host intclass=0x03 intsubclass=0x01 intprotocol=0x01 "
        "at bus=0 hubaddr=1 port=4 devaddr=5 interface=0 ugen=ugen0.5\n"
        "          ums0 pnpinfo vendor=0x046d product=0xc52b devclass=0x00 devsubclass=0x00 devproto=0x00 "
        "sernum=\"\" release=0x1211 mode=host intclass=0x03 intsubclass=0x01 intprotocol=0x02 "
        "at bus=0 hubaddr=1 port=4 devaddr=5 interface=1 ugen=ugen0.5\n");
    ASSERT_EQ(devs.size(), 2u);                       // ums0 is ukbd0's second interface
    EXPECT_EQ(devs[0].driver, "umass0");
    EXPECT_EQ(devs[0].bus, 0u);
    EXPECT_EQ(devs[0].addr, 3u);
    EXPECT_EQ(devs[0].vid, 0x0781u);
    EXPECT_EQ(devs[0].cls, "MSC");
    EXPECT_EQ(devs[0].serial, "4C530001231218115283");
    EXPECT_EQ(devs[1].driver, "ukbd0");
    EXPECT_EQ(devs[1].cls, "HID");
    EXPECT_EQ(fbx::xserver::usb::descProduct("SanDisk Cruzer Blade, class 0/0, rev 2.00/1.26, addr 3"),
              "SanDisk Cruzer Blade");
}

TEST(UsbDevinfo, GeomDisksCarrySerialAndSize) {
    auto disks = fbx::xserver::geom::parseDisks(
        "Geom name: nda0\n   Mediasize: 256060514304 (238G)\n   descr: SAMSUNG MZVLW256HEHP-00000\n   ident: S35ENX0J\n"
        "Geom name: da0\n   Mediasize: 15728640000 (15G)\n   descr: SanDisk Cruzer Blade\n   ident: 4C530001231218115283\n");
    ASSERT_EQ(disks.size(), 2u);
    EXPECT_EQ(disks[1].name, "da0");
    EXPECT_EQ(disks[1].ident, "4C530001231218115283");
    EXPECT_EQ(disks[1].bytes >> 20, 15000u);
}
