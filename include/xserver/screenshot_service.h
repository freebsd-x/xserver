#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include "xserver/vulkan_compositor.h"
#include <memory>
#include <atomic>
#include <thread>
#include <string>

namespace fbx::xserver {

class ScreenshotService {
public:
    static ScreenshotService& getInstance() noexcept;

    // Set the compositor reference used by captureToShm().
    void setCompositor(VulkanCompositor* comp) noexcept { comp_ = comp; }

    // Capture full-screen PNG on behalf of a keyboard shortcut (no IPC client).
    // Saves to ~/Pictures/Screenshots/screenshot-YYYYMMDD-HHMMSS.png.
    // Broadcasts ScreenshotTaken when done.
    void captureFullScreen(uint32_t output_idx = 0) noexcept;

    // Broadcast the region-selector trigger (Shift+Print).
    // The shell handles the overlay; when the user finishes selecting, it calls
    // ProcessMessage(CaptureRegion) with the region coordinates.
    void triggerRegionSelector() noexcept;

    // Handle incoming XBus messages for com.freebsdx.Screenshot.
    bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                        const shared::XBusMessage& msg) noexcept;

private:
    ScreenshotService() noexcept = default;
    ~ScreenshotService() noexcept;

    // Shared capture implementation: reads SHM frame, encodes PNG, saves to path_.
    // Returns true and fills path_out (≤251 chars) on success.
    bool doCapture(uint32_t output_idx,
                   int32_t crop_x, int32_t crop_y,
                   int32_t crop_w, int32_t crop_h,
                   const char* save_path_hint,
                   char* path_out, size_t path_out_sz) noexcept;

    std::string makeAutoPath() noexcept;
    void        broadcastTaken(const char* path, bool ok) noexcept;
    void        broadcastRecordingState() noexcept;

    // Recording thread
    void recordingLoop(uint32_t output_idx, uint32_t fps,
                       std::string frames_dir) noexcept;

    VulkanCompositor* comp_{nullptr};

    std::atomic<bool>     recording_{false};
    std::atomic<uint32_t> record_frame_count_{0};
    std::atomic<uint32_t> record_fps_{10};
    std::string           record_output_path_;
    std::thread           record_thread_;
    std::mutex            record_mutex_;
};

} // namespace fbx::xserver
