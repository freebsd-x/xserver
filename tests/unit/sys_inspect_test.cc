// Unit tests for pure system-inspection helpers.

#include "xserver/sys_inspect.h"
#include <gtest/gtest.h>
#include <cstring>

using namespace fbx::xserver::sysinspect;

TEST(SysInspect, OsVersion) {
    EXPECT_EQ(osVersion("FreeBSD", "14.2-RELEASE-p3"), "FreeBSD 14.2-RELEASE-p3");
    EXPECT_EQ(osVersion("", "14.2"), "14.2");        // tolerant of empty parts
    EXPECT_EQ(osVersion("FreeBSD", ""), "FreeBSD");
    EXPECT_EQ(osVersion("", ""), "");
}

static fbx::shared::VolumeEntry vol(const char* mount) {
    fbx::shared::VolumeEntry e{};
    std::strncpy(e.mount_, mount, sizeof(e.mount_) - 1);
    return e;
}

TEST(SysInspect, BuildVolumePagePaging) {
    std::vector<fbx::shared::VolumeEntry> v{vol("/"), vol("/home"), vol("/tmp"), vol("/var")};
    auto p0 = buildVolumePage(v, 0);
    EXPECT_EQ(p0.count_, 3u);
    EXPECT_EQ(p0.total_, 4u);
    EXPECT_EQ(p0.has_more_, 1u);
    EXPECT_STREQ(p0.entries_[0].mount_, "/");

    auto p1 = buildVolumePage(v, 1);
    EXPECT_EQ(p1.count_, 1u);
    EXPECT_EQ(p1.has_more_, 0u);
    EXPECT_STREQ(p1.entries_[0].mount_, "/var");
}

TEST(SysInspect, BuildVolumePageEmptyAndOutOfRange) {
    EXPECT_EQ(buildVolumePage({}, 0).count_, 0u);
    auto p = buildVolumePage({vol("/")}, 9);
    EXPECT_EQ(p.count_, 0u);
    EXPECT_EQ(p.total_, 1u);
    EXPECT_EQ(p.has_more_, 0u);
}
