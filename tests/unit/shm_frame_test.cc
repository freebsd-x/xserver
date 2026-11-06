#include "xserver/shm_frame.h"
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace fbx::xserver::shmframe;

namespace {
int memfile(off_t size, int seals) {
    int fd = ::memfd_create("shm-frame-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    EXPECT_EQ(::ftruncate(fd, size), 0);
    if (seals) { EXPECT_EQ(::fcntl(fd, F_ADD_SEALS, seals), 0); }
    return fd;
}
}  // namespace

TEST(ShmFrame, FrameBytes) {
    EXPECT_EQ(frameBytes(2, 3), 24u);
    EXPECT_EQ(frameBytes(0, 3), 0u);
    EXPECT_EQ(frameBytes(3, 0), 0u);
    EXPECT_EQ(frameBytes(kMaxDimension + 1, 1), 0u);
    EXPECT_EQ(frameBytes(1, kMaxDimension + 1), 0u);
    EXPECT_EQ(frameBytes(kMaxDimension, kMaxDimension), size_t{kMaxDimension} * kMaxDimension * 4);
}

TEST(ShmFrame, SealedAndLargeEnough) {
    int fd = memfile(4096, F_SEAL_SHRINK);
    ASSERT_GE(fd, 0);
    EXPECT_TRUE(safeToMap(fd, 4096));
    EXPECT_TRUE(safeToMap(fd, 16));
    ::close(fd);
}

TEST(ShmFrame, RefusesAFileThatCouldShrink) {
    int fd = memfile(4096, 0);
    ASSERT_GE(fd, 0);
    EXPECT_FALSE(safeToMap(fd, 16));
    ::close(fd);
    fd = memfile(4096, F_SEAL_GROW);     // sealed, but not against the thing that matters
    ASSERT_GE(fd, 0);
    EXPECT_FALSE(safeToMap(fd, 16));
    ::close(fd);
}

TEST(ShmFrame, RefusesAFileSmallerThanClaimed) {
    int fd = memfile(16, F_SEAL_SHRINK);
    ASSERT_GE(fd, 0);
    EXPECT_FALSE(safeToMap(fd, 17));
    ::close(fd);
}

TEST(ShmFrame, RefusesWhatIsntAMemoryFile) {
    int p[2];
    ASSERT_EQ(::pipe(p), 0);
    EXPECT_FALSE(safeToMap(p[0], 16));
    ::close(p[0]);
    ::close(p[1]);
    EXPECT_FALSE(safeToMap(-1, 16));
    int fd = memfile(16, F_SEAL_SHRINK);
    ASSERT_GE(fd, 0);
    EXPECT_FALSE(safeToMap(fd, 0));
    ::close(fd);
}
