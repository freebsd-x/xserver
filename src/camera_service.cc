#include "xserver/camera_service.h"
#include "xserver/async_shell.h"
#include "xserver/privacy_service.h"
#include "xserver/shell_util.h"
#include "shared/protocol.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
// V4L2 headers
#ifdef __linux__
#  include <linux/videodev2.h>
#else
// Minimal V4L2 compatibility stubs for FreeBSD (webcamd exposes /dev/videoN)
#  include <sys/ioccom.h>
#  define V4L2_CAP_VIDEO_CAPTURE  0x00000001
#  define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
struct v4l2_capability {
    uint8_t  driver[16];
    uint8_t  card[32];
    uint8_t  bus_info[32];
    uint32_t version;
    uint32_t capabilities;
    uint32_t device_caps;
    uint32_t reserved[3];
};
struct v4l2_pix_format {
    uint32_t width;
    uint32_t height;
    uint32_t pixelformat;
    uint32_t field;
    uint32_t bytesperline;
    uint32_t sizeimage;
    uint32_t colorspace;
    uint32_t priv;
    uint32_t flags;
    uint32_t ycbcr_enc;
    uint32_t quantization;
    uint32_t xfer_func;
};
struct v4l2_format {
    uint32_t type;
    union { struct v4l2_pix_format pix; uint8_t raw_data[200]; } fmt;
};
#  define VIDIOC_QUERYCAP  _IOR('V',  0, struct v4l2_capability)
#  define VIDIOC_G_FMT     _IOWR('V', 4, struct v4l2_format)
#  define VIDIOC_S_FMT     _IOWR('V', 5, struct v4l2_format)
#endif

namespace fbx::xserver {

std::string CameraService::current_device_;
uint32_t    CameraService::current_width_  = 640;
uint32_t    CameraService::current_height_ = 480;
bool        CameraService::recording_      = false;
std::string CameraService::record_path_;
pid_t       CameraService::record_pid_     = -1;

void CameraService::init() noexcept {
    // Nothing to start — lazy device open
}

std::vector<std::string> CameraService::enumerateDevices() noexcept {
    std::vector<std::string> devs;
    for (int i = 0; i < 8; ++i) {
        std::string path = std::format("/dev/video{}", i);
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        // Verify it's a V4L2 capture device
        struct v4l2_capability cap{};
        if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
            (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
        {
            devs.push_back(path);
        }
        ::close(fd);
    }
    return devs;
}

shared::CameraDevicePage CameraService::listDevices(uint32_t page) noexcept {
    shared::CameraDevicePage pg{};
    auto devs = enumerateDevices();
    pg.total_    = static_cast<uint32_t>(devs.size());
    pg.page_     = page;
    pg.valid_    = 0;
    pg.has_more_ = 0;

    if (page < devs.size()) {
        const std::string& path = devs[page];
        std::strncpy(pg.path_, path.c_str(), sizeof(pg.path_) - 1);

        // Query device name via V4L2
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            struct v4l2_capability cap{};
            if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
                std::strncpy(pg.name_,
                    reinterpret_cast<const char*>(cap.card),
                    sizeof(pg.name_) - 1);
            }
            // Query default frame size
            struct v4l2_format fmt{};
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (::ioctl(fd, VIDIOC_G_FMT, &fmt) == 0) {
                pg.width_  = fmt.fmt.pix.width;
                pg.height_ = fmt.fmt.pix.height;
            } else {
                pg.width_ = 640; pg.height_ = 480;
            }
            ::close(fd);
        }
        if (pg.name_[0] == '\0')
            std::strncpy(pg.name_, "Video Capture Device", sizeof(pg.name_) - 1);

        pg.valid_    = 1;
        pg.has_more_ = (page + 1 < devs.size()) ? 1 : 0;
    }
    return pg;
}

std::string CameraService::timestampedPath(const std::string& dir,
                                            const std::string& prefix,
                                            const std::string& ext) noexcept {
    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&t));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir + "/" + prefix + "_" + ts + "." + ext;
}

shared::CameraState CameraService::openDevice(const shared::CameraOpenRequest& req) noexcept {
    // Privacy: a global camera block denies access — report the device as not open.
    if (PrivacyService::cameraBlocked()) {
        shared::CameraState st{};
        st.open_ = 0;
        return st;
    }
    current_device_ = req.path_[0] ? std::string(req.path_, strnlen(req.path_, sizeof(req.path_))) : "/dev/video0";
    current_width_  = req.width_  ? req.width_  : 640;
    current_height_ = req.height_ ? req.height_ : 480;

    shared::CameraState st{};
    st.open_   = 1;
    st.width_  = current_width_;
    st.height_ = current_height_;
    std::strncpy(st.path_, current_device_.c_str(), sizeof(st.path_) - 1);
    return st;
}

shared::CameraState CameraService::closeDevice() noexcept {
    if (recording_) stopRecording();
    current_device_.clear();
    shared::CameraState st{};
    st.open_ = 0;
    return st;
}

shared::CameraState CameraService::getState() noexcept {
    shared::CameraState st{};
    st.open_      = current_device_.empty() ? 0 : 1;
    st.width_     = current_width_;
    st.height_    = current_height_;
    st.recording_ = recording_ ? 1 : 0;
    std::strncpy(st.path_, current_device_.c_str(), sizeof(st.path_) - 1);
    std::strncpy(st.record_path_, record_path_.c_str(), sizeof(st.record_path_) - 1);
    return st;
}

shared::CameraStillResult CameraService::captureStill(const shared::CameraStillRequest& req) noexcept {
    shared::CameraStillResult res{};
    if (PrivacyService::cameraBlocked()) return res;   // denied (success_ = 0)
    const std::string& dev = current_device_.empty() ? "/dev/video0" : current_device_;

    std::string dir = req.output_dir_[0]
        ? std::string(req.output_dir_, strnlen(req.output_dir_, sizeof(req.output_dir_)))
        : std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/Pictures";

    std::string out = timestampedPath(dir, "camera", "jpg");

    // Use ffmpeg to grab one frame
    // dev and out derive from client input (device path, output dir) — quote them.
    std::string cmd = std::format(
        "ffmpeg -f v4l2 -video_size {}x{} -i {} -vframes 1 -q:v 2 {} 2>/dev/null",
        current_width_, current_height_, shellQuote(dev), shellQuote(out));

    // Cap the capture: a busy/wedged camera device must not hang the compositor.
    int ret = fbx::xserver::spawnStatus(("timeout -k 2 20 " + cmd).c_str());
    if (ret == 0 && std::filesystem::exists(out)) {
        res.success_ = 1;
        std::strncpy(res.path_, out.c_str(), sizeof(res.path_) - 1);
    }
    return res;
}

shared::CameraState CameraService::startRecording(const shared::CameraRecordRequest& req) noexcept {
    if (PrivacyService::cameraBlocked()) return getState();   // denied: no recording
    if (recording_) stopRecording();

    const std::string& dev = current_device_.empty() ? "/dev/video0" : current_device_;
    std::string dir = req.output_dir_[0]
        ? std::string(req.output_dir_, strnlen(req.output_dir_, sizeof(req.output_dir_)))
        : std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/Videos";

    record_path_ = timestampedPath(dir, "camera", "mp4");

    // Fork ffmpeg for recording
    pid_t pid = fork();
    if (pid == 0) {
        // child
        std::string cmd = std::format(
            "ffmpeg -f v4l2 -video_size {}x{} -i {} -c:v libx264 -preset ultrafast {}",
            current_width_, current_height_, shellQuote(dev), shellQuote(record_path_));
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(1);
    }
    record_pid_ = pid;
    recording_  = true;

    return getState();
}

shared::CameraRecordResult CameraService::stopRecording() noexcept {
    shared::CameraRecordResult res{};
    if (!recording_) return res;

    if (record_pid_ > 0) {
        ::kill(record_pid_, SIGTERM);
        int status = 0;
        ::waitpid(record_pid_, &status, 0);
        record_pid_ = -1;
    }
    recording_ = false;

    if (std::filesystem::exists(record_path_)) {
        res.success_ = 1;
        std::strncpy(res.path_, record_path_.c_str(), sizeof(res.path_) - 1);
    }
    record_path_.clear();
    return res;
}

} // namespace fbx::xserver
