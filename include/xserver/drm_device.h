#pragma once

#include "xserver/unique_fd.h"
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <expected>
#include <array>
#include <system_error>
#include <functional>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace fbx::xserver {

class ISeatManager;

struct DrmProperty {
    uint32_t id_;
    uint64_t value_;
};

struct DrmObject {
    uint32_t id_;
    uint32_t type_;
    std::unordered_map<std::string, DrmProperty> props_;
};

struct ConnectorInfo {
    uint32_t width_;
    uint32_t height_;
    uint32_t refresh_rate_;
    bool connected_;
};

// DRM dumb buffer used as a hardware cursor image.
struct CursorPlane {
    uint32_t gem_handle{0};
    uint32_t pitch{0};
    void*    mapped{nullptr};
    int32_t  hot_x{1};
    int32_t  hot_y{1};
};

// Logical display output: one connector → one CRTC → one primary plane.
struct Output {
    uint32_t connector_id{0};
    uint32_t crtc_id{0};
    uint32_t plane_id{0};
    uint32_t crtc_index{0};   // index into crtcs_ vector (for property lookup)
    uint32_t plane_index{0};  // index into planes_ vector (for property lookup)
    ConnectorInfo mode{};
    bool active{false};
    bool     vrr_enabled{false};        // current VRR state (mirrors kernel CRTC VRR_ENABLED)
    bool     hdr_enabled{false};        // current HDR state
    uint32_t hdr_metadata_blob_id{0};   // DRM blob ID (non-zero when HDR active)
};

// Called after each DRM page flip completes with the kernel-provided timestamps.
using DrmFlipCallback = std::function<void(uint32_t crtc_id, uint32_t sequence,
                                           uint32_t tv_sec, uint32_t tv_usec)>;

class DrmDevice {
public:
    static std::expected<std::unique_ptr<DrmDevice>, std::error_code> create(ISeatManager& seat_manager) noexcept;

    explicit DrmDevice(UniqueFd fd, ISeatManager* seat_manager, int device_id) noexcept;
    ~DrmDevice() noexcept;

    DrmDevice(const DrmDevice&) = delete;
    DrmDevice& operator=(const DrmDevice&) = delete;
    DrmDevice(DrmDevice&&) = delete;
    DrmDevice& operator=(DrmDevice&&) = delete;

    [[nodiscard]] int fd() const noexcept { return fd_.get(); }

    std::expected<void, std::error_code> discoverResources() noexcept;
    std::expected<void, std::error_code> rescan() noexcept;

    ConnectorInfo getNativeResolution(uint32_t connector_id) noexcept;

    [[nodiscard]] drmModeAtomicReqPtr allocateAtomicReq() const noexcept;
    [[nodiscard]] bool commitAtomic(drmModeAtomicReqPtr req, uint32_t flags, void* user_data) noexcept;
    
    void fetchObjectProperties(DrmObject& obj) noexcept;
    [[nodiscard]] uint32_t getPropertyId(const DrmObject& obj, const std::string& prop_name) const noexcept;

    [[nodiscard]] std::expected<uint32_t, std::error_code> createDumbBuffer(uint32_t width, uint32_t height, uint32_t bpp, uint32_t& pitch, uint64_t& size, uint32_t& handle) noexcept;
    [[nodiscard]] std::expected<uint32_t, std::error_code> addFramebuffer(uint32_t width, uint32_t height, uint32_t pixel_format, uint32_t handle, uint32_t pitch) noexcept;
    [[nodiscard]] std::expected<uint32_t, std::error_code> importFdAndAddFb(int client_fd, uint32_t width, uint32_t height, uint32_t pixel_format, uint32_t pitch) noexcept;
    void handleEvent() noexcept;

    // Register a callback invoked from handleEvent() for every completed page flip.
    void setFlipCallback(DrmFlipCallback cb) noexcept { flip_callback_ = std::move(cb); }

    // Flip a Framebuffer to the primary plane of output 0 (backward-compat wrapper)
    [[nodiscard]] std::expected<void, std::error_code> flip(uint32_t fb_id, void* user_data = nullptr) noexcept;

    // Flip a Framebuffer to a specific output by index
    [[nodiscard]] std::expected<void, std::error_code> flipOutput(size_t output_idx, uint32_t fb_id, void* user_data = nullptr) noexcept;

    // Physically power on all connected outputs (DPMS ON)
    [[nodiscard]] std::expected<void, std::error_code> forceEnableDisplay() noexcept;

    // VRR (Variable Refresh Rate) — requires DRM atomic + CRTC VRR_ENABLED property.
    // isVrrCapable() returns true when the CRTC exposes the VRR_ENABLED property.
    // setVrrEnabled() issues a non-modeset atomic commit to toggle VRR.
    [[nodiscard]] bool isVrrCapable(size_t output_idx) const noexcept;
    [[nodiscard]] std::expected<void, std::error_code> setVrrEnabled(size_t output_idx, bool enable) noexcept;

    // Refresh rate (com.freebsdx.RefreshRate). supportedRefreshMask: bitmask of
    // {bit0=60,bit1=120,bit2=144,bit3=240} Hz modes this output offers at its
    // current resolution (always includes 60 as a floor). setRefreshRate: modeset
    // to a same-resolution mode at the requested Hz (ALLOW_MODESET atomic commit);
    // fails cleanly if no such mode exists.
    [[nodiscard]] uint32_t supportedRefreshMask(size_t output_idx) noexcept;
    [[nodiscard]] std::expected<void, std::error_code> setRefreshRate(size_t output_idx, uint32_t hz) noexcept;

    // Colour transform (com.freebsdx.ColorFilter): set the CRTC's 3x3 CTM from a
    // row-major matrix of non-negative coefficients. No-op (invalid_argument) when
    // the CRTC lacks the CTM property. Non-modeset atomic commit.
    std::expected<void, std::error_code> setColorMatrix(size_t output_idx,
                                                        const std::array<double, 9>& m) noexcept;

    // HDR output — requires DRM atomic + connector HDR_OUTPUT_METADATA property.
    // isHdrCapable() checks if the connector exposes the HDR_OUTPUT_METADATA property.
    // setHdrEnabled() creates/destroys a metadata blob and commits via atomic.
    [[nodiscard]] bool isHdrCapable(size_t output_idx) const noexcept;
    [[nodiscard]] std::expected<void, std::error_code> setHdrEnabled(
        size_t output_idx, bool enable, uint32_t max_luminance_nits = 1000) noexcept;

    // Hardware cursor plane (DRM_IOCTL_MODE_CURSOR2 / drmModeMoveCursor)
    // initCursor() allocates a 64×64 dumb buffer, draws an arrow at the given
    // display scale factor, and uploads it to every active CRTC.
    // Must be called after forceEnableDisplay().
    std::expected<void, std::error_code> initCursor(float scale = 1.0f) noexcept;
    // Redraw the cursor bitmap for the given wp_cursor_shape_device_v1 shape enum value
    // and re-upload it to all active CRTCs. Falls back to the arrow for unknown shapes.
    void setCursorShape(uint32_t shape, float scale = 1.0f) noexcept;
    // Move the cursor to (x, y) in CRTC-local coordinates for a given output.
    void moveCursor(size_t output_idx, int32_t x, int32_t y) noexcept;
    // Hide the cursor on a specific output (handle=0 disables the hardware plane).
    void hideCursor(size_t output_idx) noexcept;
    // Re-show a previously hidden cursor using the current cursor image.
    void showCursor(size_t output_idx) noexcept;
    // Hide / show the cursor on ALL active outputs at once.
    void hideCursorAll() noexcept;
    void showCursorAll() noexcept;

    [[nodiscard]] const std::vector<DrmObject>& connectors() const noexcept { return connectors_; }
    [[nodiscard]] const std::vector<DrmObject>& crtcs() const noexcept { return crtcs_; }
    [[nodiscard]] const std::vector<DrmObject>& planes() const noexcept { return planes_; }
    [[nodiscard]] const std::vector<Output>& outputs() const noexcept { return outputs_; }

    // Restore each taken-over CRTC to the mode/framebuffer it had before the
    // compositor started (the text console / fbcon scanout). Called from the
    // destructor before drmDropMaster so the seat is handed back in a usable
    // state instead of a black/frozen last-frame. Idempotent.
    void restoreSavedCrtcs() noexcept;

private:
    std::expected<void, std::error_code> enableAtomic() noexcept;
    void buildOutputAssignments(drmModeRes* res) noexcept;

    // Original CRTC scanout state captured before the first modeset, so it can
    // be reinstated on shutdown. Owns the drmModeCrtc* (freed on restore).
    struct SavedCrtc {
        uint32_t     crtc_id{0};
        uint32_t     connector_id{0};
        drmModeCrtc* crtc{nullptr};
    };
    std::vector<SavedCrtc> saved_crtcs_;

    static constexpr uint32_t kCursorSize = 64;

    // Per-output context passed as user_data through the DRM flip event pipeline.
    // Stored here (heap-stable via unique_ptr) so the pointer remains valid between
    // flipOutput() and the corresponding handleEvent() callback.
    struct FlipCtx {
        DrmDevice* self{nullptr};
        bool*      pending_flag{nullptr}; // nullptr for secondary outputs
    };
    std::vector<std::unique_ptr<FlipCtx>> flip_contexts_;
    DrmFlipCallback                        flip_callback_;

    UniqueFd fd_;
    ISeatManager* seat_manager_;
    int device_id_{-1};
    CursorPlane cursor_{};

    std::vector<DrmObject> connectors_;
    std::vector<DrmObject> crtcs_;
    std::vector<DrmObject> planes_;
    std::vector<Output> outputs_;
};

} // namespace fbx::xserver
