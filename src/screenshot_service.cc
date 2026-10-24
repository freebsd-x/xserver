#include "xserver/screenshot_service.h"
#include "xserver/async_shell.h"
#include <iostream>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <png.h>
#include <mutex>
#include <vector>
#include <chrono>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

ScreenshotService& ScreenshotService::getInstance() noexcept {
    static ScreenshotService instance;
    return instance;
}

ScreenshotService::~ScreenshotService() noexcept {
    recording_.store(false);
    if (record_thread_.joinable()) record_thread_.join();
}

// ── Path helpers ──────────────────────────────────────────────────────────────

std::string ScreenshotService::makeAutoPath() noexcept {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";

    // Ensure ~/Pictures/Screenshots/ exists
    std::string dir = std::string(home) + "/Pictures/Screenshots";
    ::mkdir((std::string(home) + "/Pictures").c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);

    // Timestamp suffix
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);

    return dir + "/screenshot-" + ts + ".png";
}

// ── PNG encode ─────────────────────────────────────────────────────────────────

// Encodes XRGB8888 pixels (stride = width*4) to a PNG file.
// If crop_w/crop_h are 0, encode the full frame.
static bool writePng(const char* path,
                     const uint8_t* data,
                     uint32_t full_w, uint32_t full_h, uint32_t stride,
                     int32_t cx, int32_t cy,
                     uint32_t cw, uint32_t ch) noexcept {
    // Clamp crop to frame
    if (cw == 0 || ch == 0) { cx = 0; cy = 0; cw = full_w; ch = full_h; }
    if (cx < 0) { cw = (cw > (uint32_t)(-cx)) ? cw + cx : 0; cx = 0; }
    if (cy < 0) { ch = (ch > (uint32_t)(-cy)) ? ch + cy : 0; cy = 0; }
    if ((uint32_t)cx >= full_w || (uint32_t)cy >= full_h || cw == 0 || ch == 0) return false;
    if ((uint32_t)cx + cw > full_w) cw = full_w - (uint32_t)cx;
    if ((uint32_t)cy + ch > full_h) ch = full_h - (uint32_t)cy;

    FILE* fp = std::fopen(path, "wb");
    if (!fp) return false;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                              nullptr, nullptr, nullptr);
    if (!png) { std::fclose(fp); return false; }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, nullptr);
        std::fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, cw, ch, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    // Convert XRGB8888 → RGB row by row
    std::vector<uint8_t> row(cw * 3);
    for (uint32_t y = 0; y < ch; ++y) {
        const uint8_t* src = data + ((uint32_t)cy + y) * stride + (uint32_t)cx * 4;
        for (uint32_t x = 0; x < cw; ++x) {
            // XRGB8888: byte order B, G, R, X (little-endian uint32)
            row[x * 3 + 0] = src[x * 4 + 2]; // R
            row[x * 3 + 1] = src[x * 4 + 1]; // G
            row[x * 3 + 2] = src[x * 4 + 0]; // B
        }
        png_write_row(png, row.data());
    }

    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(fp);
    return true;
}

// ── Core capture ──────────────────────────────────────────────────────────────

bool ScreenshotService::doCapture(uint32_t output_idx,
                                   int32_t crop_x, int32_t crop_y,
                                   int32_t crop_w, int32_t crop_h,
                                   const char* save_path_hint,
                                   char* path_out, size_t path_out_sz) noexcept {
    if (!comp_) {
        std::cerr << "[ScreenshotService] No compositor set.\n";
        return false;
    }

    auto res = comp_->captureToShm(output_idx);
    if (!res.has_value()) {
        std::cerr << "[ScreenshotService] captureToShm failed.\n";
        return false;
    }

    auto [shm_fd, info] = std::move(res.value());
    const size_t shm_sz = info.stride_ * info.height_;

    const uint8_t* pixels = static_cast<const uint8_t*>(
        ::mmap(nullptr, shm_sz, PROT_READ, MAP_SHARED, shm_fd, 0));
    ::close(shm_fd);

    if (pixels == MAP_FAILED) {
        std::cerr << "[ScreenshotService] mmap of SHM failed.\n";
        return false;
    }

    std::string path = (save_path_hint && save_path_hint[0] != '\0')
                       ? std::string(save_path_hint)
                       : makeAutoPath();

    bool ok = writePng(path.c_str(), pixels,
                       info.width_, info.height_, info.stride_,
                       crop_x, crop_y,
                       static_cast<uint32_t>(crop_w > 0 ? crop_w : 0),
                       static_cast<uint32_t>(crop_h > 0 ? crop_h : 0));
    ::munmap(const_cast<uint8_t*>(pixels), shm_sz);

    if (ok) {
        std::snprintf(path_out, path_out_sz, "%s", path.c_str());
        std::cout << "[ScreenshotService] Saved: " << path << "\n";
    }
    return ok;
}

// ── Public capture helpers ────────────────────────────────────────────────────

void ScreenshotService::captureFullScreen(uint32_t output_idx) noexcept {
    char path[252]{};
    bool ok = doCapture(output_idx, 0, 0, 0, 0, nullptr, path, sizeof(path));
    broadcastTaken(path, ok);
}

void ScreenshotService::triggerRegionSelector() noexcept {
    // Broadcast a ScreenshotCaptureRegion XBus message; xshell creates the overlay.
    shared::XBusMessage msg{};
    msg.magic_ = shared::XBUS_MAGIC;
    std::strncpy(msg.service_name_, "com.freebsdx.Screenshot",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::ScreenshotOpcode::CaptureRegion);
    // payload empty — shell interprets this as "show selector"
    BroadcastXBusMessage(msg);
}

// ── Broadcasts ────────────────────────────────────────────────────────────────

void ScreenshotService::broadcastTaken(const char* path, bool ok) noexcept {
    shared::XBusMessage msg{};
    msg.magic_ = shared::XBUS_MAGIC;
    std::strncpy(msg.service_name_, "com.freebsdx.Screenshot",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::ScreenshotOpcode::ScreenshotTaken);

    shared::ScreenshotResponse resp{};
    resp.success_ = ok ? 1u : 0u;
    if (path) std::strncpy(resp.path_, path, sizeof(resp.path_) - 1);
    std::memcpy(msg.payload_, &resp, sizeof(resp));
    BroadcastXBusMessage(msg);
}

void ScreenshotService::broadcastRecordingState() noexcept {
    shared::XBusMessage msg{};
    msg.magic_ = shared::XBUS_MAGIC;
    std::strncpy(msg.service_name_, "com.freebsdx.Screenshot",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::ScreenshotOpcode::RecordingChanged);

    shared::ScreenshotRecordingState st{};
    st.recording_    = recording_.load() ? 1u : 0u;
    st.frame_count_  = record_frame_count_.load();
    st.fps_          = record_fps_.load();
    {
        std::lock_guard<std::mutex> lock(record_mutex_);
        std::strncpy(st.path_, record_output_path_.c_str(), sizeof(st.path_) - 1);
    }
    std::memcpy(msg.payload_, &st, sizeof(st));
    BroadcastXBusMessage(msg);
}

// ── Screen recording ──────────────────────────────────────────────────────────

void ScreenshotService::recordingLoop(uint32_t output_idx, uint32_t fps,
                                       std::string frames_dir) noexcept {
    using namespace std::chrono;
    const auto frame_interval = microseconds(1'000'000u / std::max(fps, 1u));

    while (recording_.load()) {
        auto t0 = steady_clock::now();

        char frame_path[512];
        std::snprintf(frame_path, sizeof(frame_path), "%s/frame-%06u.png",
                      frames_dir.c_str(),
                      record_frame_count_.load());
        char tmp[252]{};
        if (doCapture(output_idx, 0, 0, 0, 0, frame_path, tmp, sizeof(tmp)))
            record_frame_count_.fetch_add(1);

        auto elapsed = steady_clock::now() - t0;
        if (elapsed < frame_interval)
            std::this_thread::sleep_for(frame_interval - elapsed);
    }

    // Mux frames with ffmpeg if available
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    std::string out_path;
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        ::localtime_r(&t, &tm);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
        out_path = std::string(home) + "/Videos/recording-" + ts + ".mp4";
        ::mkdir((std::string(home) + "/Videos").c_str(), 0755);
    }

    char ffcmd[1024];
    std::snprintf(ffcmd, sizeof(ffcmd),
        "ffmpeg -y -r %u -i '%s/frame-%%06d.png' -c:v libx264 -pix_fmt yuv420p '%s' "
        ">/dev/null 2>&1 && rm -rf '%s'",
        fps, frames_dir.c_str(), out_path.c_str(), frames_dir.c_str());
    (void)spawnStatus(ffcmd); // best-effort (posix_spawn, fork-safe — see async_shell.h); frames stay if ffmpeg absent

    {
        std::lock_guard<std::mutex> lock(record_mutex_);
        record_output_path_ = out_path;
    }
    std::cout << "[ScreenshotService] Recording saved: " << out_path << "\n";
    broadcastRecordingState();
}

// ── ProcessMessage ────────────────────────────────────────────────────────────

bool ScreenshotService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                        const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.Screenshot") != 0) return false;

    using namespace shared;
    XBusMessage response = msg;

    switch (static_cast<ScreenshotOpcode>(msg.opcode_)) {

    case ScreenshotOpcode::Capture: {
        ScreenshotRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.save_path_[sizeof(req.save_path_) - 1] = '\0';

        char path[252]{};
        bool ok = doCapture(req.output_idx_, 0, 0, 0, 0,
                            req.save_path_, path, sizeof(path));
        broadcastTaken(path, ok);

        ScreenshotResponse resp{};
        resp.success_ = ok ? 1u : 0u;
        std::strncpy(resp.path_, path, sizeof(resp.path_) - 1);
        std::memcpy(response.payload_, &resp, sizeof(resp));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case ScreenshotOpcode::CaptureRegion: {
        ScreenshotRegionRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.save_path_[sizeof(req.save_path_) - 1] = '\0';

        // If width/height are both 0 the shell is asking us to trigger the
        // selector overlay, not to actually capture yet.
        if (req.width_ == 0 && req.height_ == 0) {
            triggerRegionSelector();
            (void)client->sendMessage(
                {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
            break;
        }

        char path[252]{};
        bool ok = doCapture(req.output_idx_,
                            req.x_, req.y_,
                            static_cast<int32_t>(req.width_),
                            static_cast<int32_t>(req.height_),
                            req.save_path_, path, sizeof(path));
        broadcastTaken(path, ok);

        ScreenshotResponse resp{};
        resp.success_ = ok ? 1u : 0u;
        std::strncpy(resp.path_, path, sizeof(resp.path_) - 1);
        std::memcpy(response.payload_, &resp, sizeof(resp));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case ScreenshotOpcode::StartRecording: {
        if (recording_.load()) {
            // Already recording — just ack
            (void)client->sendMessage(
                {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
            break;
        }

        ScreenshotRecordingRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.save_dir_[sizeof(req.save_dir_) - 1] = '\0';

        uint32_t fps = (req.fps_ > 0 && req.fps_ <= 60) ? req.fps_ : 10u;
        record_fps_.store(fps);
        record_frame_count_.store(0);

        // Create a temp frames directory
        const char* home = std::getenv("HOME");
        if (!home) home = "/tmp";
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        ::localtime_r(&t, &tm);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
        std::string frames_dir = std::string(home) + "/.cache/freebsdx-recording-" + ts;
        ::mkdir(frames_dir.c_str(), 0700);

        recording_.store(true);
        {
            std::lock_guard<std::mutex> lock(record_mutex_);
            record_output_path_.clear();
        }
        record_thread_ = std::thread(&ScreenshotService::recordingLoop, this,
                                     req.output_idx_, fps, frames_dir);
        broadcastRecordingState();

        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case ScreenshotOpcode::StopRecording: {
        if (recording_.load()) {
            recording_.store(false);
            if (record_thread_.joinable()) record_thread_.join();
        }
        // broadcastRecordingState() already called at end of recordingLoop.

        shared::ScreenshotRecordingState st{};
        st.recording_   = 0;
        st.frame_count_ = record_frame_count_.load();
        st.fps_         = record_fps_.load();
        {
            std::lock_guard<std::mutex> lock(record_mutex_);
            std::strncpy(st.path_, record_output_path_.c_str(), sizeof(st.path_) - 1);
        }
        std::memcpy(response.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case ScreenshotOpcode::GetStatus: {
        shared::ScreenshotRecordingState st{};
        st.recording_   = recording_.load() ? 1u : 0u;
        st.frame_count_ = record_frame_count_.load();
        st.fps_         = record_fps_.load();
        {
            std::lock_guard<std::mutex> lock(record_mutex_);
            std::strncpy(st.path_, record_output_path_.c_str(), sizeof(st.path_) - 1);
        }
        std::memcpy(response.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    default:
        return false;
    }

    return true;
}

} // namespace fbx::xserver
