#pragma once
// A frame sent as shared memory (RenderMessageStatus::ShmFrame). The compositor
// maps it, and touching a mapping whose file has since shrunk is a fatal signal,
// so the sender must have made that impossible: the file has to be sealed against
// shrinking. Only the Wayland bridge sends these, but any client could try.

#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>

namespace fbx::xserver::shmframe {

inline constexpr uint32_t kMaxDimension = 16384;

// Tightly packed RGBA8: width × height × 4, or 0 if the size isn't acceptable.
inline size_t frameBytes(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) return 0;
    return static_cast<size_t>(width) * height * 4;
}

// Is it safe to map `bytes` from this descriptor and read them?
inline bool safeToMap(int fd, size_t bytes) {
    if (fd < 0 || bytes == 0) return false;
    const int seals = ::fcntl(fd, F_GET_SEALS);
    if (seals < 0 || !(seals & F_SEAL_SHRINK)) return false;
    struct stat st{};
    return ::fstat(fd, &st) == 0 && st.st_size >= 0 && static_cast<size_t>(st.st_size) >= bytes;
}

}  // namespace fbx::xserver::shmframe
