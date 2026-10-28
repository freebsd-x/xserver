// Pure parsers/validators for NetworkShare(s), VirtualMachines, Containers.

#include "xserver/infra_parse.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::infra;

TEST(InfraShares, ParsesSmbAndNfsSpecs) {
    Share s;
    ASSERT_TRUE(parseSpec("//GUEST@NAS/MEDIA", "smbfs", s));
    EXPECT_EQ(s.protocol, kSmb);
    EXPECT_EQ(s.user, "GUEST");
    EXPECT_EQ(s.host, "NAS");
    EXPECT_EQ(s.share, "/MEDIA");
    ASSERT_TRUE(parseSpec("//fileserver/docs/2026", "smbfs", s));
    EXPECT_EQ(s.user, "");
    EXPECT_EQ(s.share, "/docs/2026");
    ASSERT_TRUE(parseSpec("nas.lan:/export/home", "nfs", s));
    EXPECT_EQ(s.protocol, kNfs);
    EXPECT_EQ(s.host, "nas.lan");
    EXPECT_EQ(s.share, "/export/home");
    EXPECT_FALSE(parseSpec("/dev/ada0p2", "ufs", s));
    EXPECT_FALSE(parseSpec("nas:relative", "nfs", s));
    EXPECT_FALSE(parseSpec("//nas", "smbfs", s));
}

TEST(InfraShares, MountedFirstThenUnmountedFstab) {
    auto shares = collectShares(
        "nas:/export/media\t/mnt/media\tnfs\trw\t0\t0\n"
        "zroot/usr/home\t/usr/home\tzfs\trw\t0\t0\n",
        "# Device\tMountpoint\tFStype\tOptions\tDump\tPass#\n"
        "/dev/gpt/efiboot0\t/boot/efi\tmsdosfs\trw\t2\t2\n"
        "nas:/export/media\t/mnt/media\tnfs\trw,noauto\t0\t0\n"
        "//me@nas/backup  /mnt/backup  smbfs  rw,noauto  0  0\n");
    ASSERT_EQ(shares.size(), 2u);
    EXPECT_EQ(shares[0].mountpoint, "/mnt/media");
    EXPECT_TRUE(shares[0].mounted);                  // not duplicated from fstab
    EXPECT_EQ(shares[1].mountpoint, "/mnt/backup");
    EXPECT_FALSE(shares[1].mounted);
    EXPECT_EQ(shares[1].protocol, kSmb);
    EXPECT_TRUE(collectShares("", "").empty());
    EXPECT_EQ(shareId("/mnt/media"), shareId("/mnt/media"));
    EXPECT_NE(shareId("/mnt/media"), shareId("/mnt/backup"));
}

TEST(InfraShares, ArgumentValidation) {
    EXPECT_TRUE(validHost("nas.lan"));
    EXPECT_TRUE(validHost("192.168.0.10"));
    EXPECT_TRUE(validHost("fe80::1"));
    EXPECT_FALSE(validHost("-oexec=/bin/sh"));
    EXPECT_FALSE(validHost("nas lan"));
    EXPECT_FALSE(validHost(""));
    EXPECT_TRUE(validSharePath("/export/media"));
    EXPECT_FALSE(validSharePath("export"));
    EXPECT_FALSE(validSharePath("/export/../etc"));
    EXPECT_FALSE(validSharePath("/a b"));
    EXPECT_TRUE(validUser(""));
    EXPECT_TRUE(validUser("wheel"));
    EXPECT_FALSE(validUser("-N"));
    EXPECT_FALSE(validUser("me@nas"));
}

TEST(InfraContainers, ParsesPodmanPsAndStates) {
    auto cs = parsePodmanPs(
        "3f2a1b9c8d7e6f50\tweb\tdocker.io/library/nginx:latest\trunning\n"
        "aaaaaaaaaaaa\tdb\tpostgres:16\texited\n"
        "bbbbbbbbbbbb\tjob\talpine\tpaused\n"
        "cccccccccccc\tweird\talpine\tremoving\n"
        "garbage\n");
    ASSERT_EQ(cs.size(), 4u);
    EXPECT_EQ(cs[0].id, "3f2a1b9c8d7e");             // short id (12)
    EXPECT_EQ(cs[0].state, 1u);
    EXPECT_EQ(cs[1].state, 0u);
    EXPECT_EQ(cs[2].state, 2u);
    EXPECT_EQ(cs[3].state, 3u);
    EXPECT_TRUE(parsePodmanPs("").empty());
    EXPECT_TRUE(validContainerId("3f2a1b9c8d7e"));
    EXPECT_FALSE(validContainerId("--all"));
    EXPECT_FALSE(validContainerId("3F2A1B9C8D7E"));
    EXPECT_FALSE(validContainerId("abc"));
}

TEST(InfraVms, NameValidation) {
    EXPECT_TRUE(validVmName("freebsd-14"));
    EXPECT_FALSE(validVmName("../x"));
    EXPECT_FALSE(validVmName("-x"));
    EXPECT_FALSE(validVmName(""));
}
