// Unit tests for PermissionStore — per-app capability grant logic + persistence.

#include "xserver/permission_store.h"
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using fbx::xserver::PermissionStore;
using Cap = PermissionStore::Capability;

namespace {
std::string tmpPath() {
    return (fs::temp_directory_path() /
            ("fbx_perms_" + std::to_string(::getpid()) + ".txt")).string();
}
}  // namespace

TEST(PermissionStoreTest, SensitiveCapsDenyByDefault) {
    PermissionStore s;
    s.load(tmpPath());
    // No stored decision: camera + mic + screen-capture deny-by-default; rest allow.
    EXPECT_FALSE(s.isAllowed("com.x.App", Cap::Camera));
    EXPECT_FALSE(s.isAllowed("com.x.App", Cap::Microphone));
    EXPECT_FALSE(s.isAllowed("com.x.App", Cap::ScreenCapture));
    EXPECT_TRUE(s.isAllowed("com.x.App", Cap::Net));
    EXPECT_TRUE(s.isAllowed("com.x.App", Cap::Filesystem));
    EXPECT_TRUE(s.isAllowed("com.x.App", Cap::Gpu));
}

TEST(PermissionStoreTest, GrantAndRevoke) {
    PermissionStore s;
    s.load(tmpPath());
    EXPECT_TRUE(s.set("com.x.Cam", Cap::Camera, true));
    EXPECT_TRUE(s.isAllowed("com.x.Cam", Cap::Camera));
    EXPECT_TRUE(s.set("com.x.Cam", Cap::Camera, false));
    EXPECT_FALSE(s.isAllowed("com.x.Cam", Cap::Camera));
}

TEST(PermissionStoreTest, ScreenCaptureGrantable) {
    PermissionStore s;
    s.load(tmpPath());
    EXPECT_FALSE(s.isAllowed("com.x.Rec", Cap::ScreenCapture));  // default deny
    s.set("com.x.Rec", Cap::ScreenCapture, true);
    EXPECT_TRUE(s.isAllowed("com.x.Rec", Cap::ScreenCapture));
}

TEST(PermissionStoreTest, ExplicitRevokeOverridesAllowDefault) {
    PermissionStore s;
    s.load(tmpPath());
    // Net defaults allowed; an explicit revoke must stick.
    s.set("com.x.Net", Cap::Net, false);
    EXPECT_FALSE(s.isAllowed("com.x.Net", Cap::Net));
}

TEST(PermissionStoreTest, PerAppIsolation) {
    PermissionStore s;
    s.load(tmpPath());
    s.set("com.x.A", Cap::Camera, true);
    EXPECT_TRUE(s.isAllowed("com.x.A", Cap::Camera));
    EXPECT_FALSE(s.isAllowed("com.x.B", Cap::Camera));  // B unaffected → default deny
}

TEST(PermissionStoreTest, PersistsAcrossReload) {
    const std::string path = tmpPath();
    fs::remove(path);
    {
        PermissionStore s;
        s.load(path);
        s.set("com.x.Cam", Cap::Camera, true);      // grant sensitive
        s.set("com.x.Cam", Cap::Net, false);        // explicit revoke of default-allow
    }
    PermissionStore s2;
    s2.load(path);
    EXPECT_TRUE(s2.isAllowed("com.x.Cam", Cap::Camera));
    EXPECT_FALSE(s2.isAllowed("com.x.Cam", Cap::Net));
    // A cap with no explicit decision still follows its default after reload.
    EXPECT_FALSE(s2.isAllowed("com.x.Cam", Cap::Microphone));
    fs::remove(path);
}

TEST(PermissionStoreTest, ListReflectsDefaultsAndGrants) {
    PermissionStore s;
    s.load(tmpPath());
    s.set("com.x.App", Cap::Microphone, true);
    auto all = s.list();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].app, "com.x.App");
    EXPECT_TRUE(all[0].allowed[Cap::Microphone]);   // granted
    EXPECT_FALSE(all[0].allowed[Cap::Camera]);      // default deny
    EXPECT_TRUE(all[0].allowed[Cap::Net]);          // default allow
}

TEST(PermissionStoreTest, RejectsBadCapability) {
    PermissionStore s;
    s.load(tmpPath());
    EXPECT_FALSE(s.set("com.x.App", static_cast<Cap>(99), true));
    EXPECT_FALSE(s.isAllowed("com.x.App", static_cast<Cap>(99)));
}
