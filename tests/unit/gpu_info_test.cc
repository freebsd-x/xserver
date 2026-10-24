// Pure parser for pciconf(8): the display-class GPU name.

#include "xserver/gpu_info.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::gpu;

namespace {
constexpr const char* kPciconf =
    "hostb0@pci0:0:0:0:\tclass=0x060000 ... device=0x5904 ...\n"
    "    vendor     = 'Intel Corporation'\n"
    "    device     = 'Xeon E3-1200 Host Bridge'\n"
    "    class      = bridge\n"
    "vgapci0@pci0:0:2:0:\tclass=0x030000 rev=0x07 ... device=0x5917 ...\n"
    "    vendor     = 'Intel Corporation'\n"
    "    device     = 'Kaby Lake-R GT2 [UHD Graphics 620]'\n"
    "    class      = display\n"
    "    subclass   = VGA\n"
    "none0@pci0:0:4:0:\tclass=0x118000 ... device=0x1903 ...\n"
    "    vendor     = 'Intel Corporation'\n"
    "    device     = 'Thermal Subsystem'\n"
    "    class      = dasp\n";
}

TEST(GpuInfo, PicksDisplayClassDeviceName) {
    EXPECT_EQ(parseGpuName(kPciconf), "Kaby Lake-R GT2 [UHD Graphics 620]");
}

TEST(GpuInfo, IgnoresHeaderDeviceIdsAndNonDisplayBlocks) {
    // No display/VGA block → no GPU name (bridge + dasp only).
    const char* noGpu =
        "hostb0@pci0:0:0:0:\tclass=0x060000 ... device=0x5904 ...\n"
        "    device     = 'Host Bridge'\n"
        "    class      = bridge\n";
    EXPECT_EQ(parseGpuName(noGpu), "");
}

TEST(GpuInfo, EmptyInput) {
    EXPECT_EQ(parseGpuName(""), "");
}

TEST(GpuInfo, FirstDisplayWinsAcrossMultipleGpus) {
    const char* two =
        "vgapci0@pci0:0:2:0:\tclass=0x030000 ... device=0x5917 ...\n"
        "    device     = 'Integrated GPU'\n"
        "    class      = display\n"
        "vgapci1@pci0:1:0:0:\tclass=0x030000 ... device=0x1234 ...\n"
        "    device     = 'Discrete GPU'\n"
        "    class      = display\n";
    EXPECT_EQ(parseGpuName(two), "Integrated GPU");
}
