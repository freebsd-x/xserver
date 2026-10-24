#pragma once
#include "shared/protocol.h"
#include <string>
#include <vector>
#include <sys/types.h>

namespace fbx::xserver {

class CameraService {
public:
    static void init() noexcept;

    // Returns one page of detected V4L2 devices
    static shared::CameraDevicePage listDevices(uint32_t page) noexcept;

    // Capture one frame from the active device to ~/Pictures/camera_TIMESTAMP.jpg
    // Returns the saved path on success, empty on failure.
    static shared::CameraStillResult captureStill(const shared::CameraStillRequest& req) noexcept;

    // Start recording from the active device to ~/Videos/camera_TIMESTAMP.mp4
    static shared::CameraState startRecording(const shared::CameraRecordRequest& req) noexcept;

    // Stop ongoing recording; returns the saved path.
    static shared::CameraRecordResult stopRecording() noexcept;

    // Open / close (tracks current device for capture operations)
    static shared::CameraState openDevice(const shared::CameraOpenRequest& req) noexcept;
    static shared::CameraState closeDevice() noexcept;

    static shared::CameraState getState() noexcept;

private:
    static std::string    current_device_;
    static uint32_t       current_width_;
    static uint32_t       current_height_;
    static bool           recording_;
    static std::string    record_path_;
    static pid_t          record_pid_;

    static std::string timestampedPath(const std::string& dir,
                                       const std::string& prefix,
                                       const std::string& ext) noexcept;
    static std::vector<std::string> enumerateDevices() noexcept;
};

} // namespace fbx::xserver
