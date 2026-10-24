#pragma once

#include "xserver/unique_fd.h"
#include "xserver/window_decorator.h"
#include "xserver/gpu_profiler.h"
#include "shared/protocol.h"
#include "xrender/font_atlas.h"
#include "xrender/text_shaper.h"
#include "xrender/font_manager.h"
#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include <array>
#include <unordered_map>
#include <expected>
#include <string>
#include <system_error>
#include <chrono>

namespace fbx::xserver {

// ── iOS-style GPU cursor spring physics ─────────────────────────────────────
struct SpringScalar {
    float pos{0.f}, vel{0.f}, target{0.f};
    void setTarget(float t) noexcept { target = t; }
    void step(float dt) noexcept {
        constexpr float k = 400.f, d = 40.f;
        float force = -k * (pos - target) - d * vel;
        vel += force * dt;
        pos += vel * dt;
    }
    void snap() noexcept { pos = target; vel = 0.f; }
};

enum class CursorAnimState : uint8_t { Default, HoverButton, TextCaret, Drag };

struct GpuCursorState {
    float raw_x{960.f}, raw_y{540.f};
    SpringScalar sx, sy;      // position springs
    SpringScalar sw, sh;      // half-size springs
    SpringScalar sr;          // corner radius spring
    SpringScalar salpha;      // fill alpha spring
    CursorAnimState anim{CursorAnimState::Default};
    // Hover target for morphing
    float hover_cx{0}, hover_cy{0}, hover_hw{0}, hover_hh{0};
    bool  has_hover{false};
};

class WindowStack;

enum class VulkanError {
    InstanceCreationFailed,
    PhysicalDeviceNotFound,
    LogicalDeviceCreationFailed,
    ExtensionNotSupported,
    ImportFailed,
    MemoryAllocationFailed
};

// Resources for one physical display output (double-buffered).
struct PerOutputTarget {
    std::vector<VkImage>        render_image{2, VK_NULL_HANDLE};
    std::vector<VkDeviceMemory> render_memory{2, VK_NULL_HANDLE};
    std::vector<VkImageView>    render_view{2, VK_NULL_HANDLE};
    std::vector<VkImage>        target_image{2, VK_NULL_HANDLE};
    std::vector<VkDeviceMemory> target_memory{2, VK_NULL_HANDLE};
    std::vector<VkImageView>    target_view{2, VK_NULL_HANDLE};
    std::vector<VkFramebuffer>  target_framebuffer{2, VK_NULL_HANDLE};
    uint32_t stride{0};
    uint32_t width{1920};
    uint32_t height{1080};
    uint32_t current_frame{0};
};

class VulkanCompositor {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<VulkanCompositor>, VulkanError> create() noexcept;

    ~VulkanCompositor() noexcept;

    VulkanCompositor(const VulkanCompositor&) = delete;
    VulkanCompositor& operator=(const VulkanCompositor&) = delete;
    VulkanCompositor(VulkanCompositor&&) noexcept;
    VulkanCompositor& operator=(VulkanCompositor&&) noexcept;

    // Imports a DMA-BUF as a sampled VkImage
    [[nodiscard]] std::expected<VkImage, VulkanError> importExternalBuffer(int fd, uint32_t width, uint32_t height) noexcept;
    // Creates a 1×1 device-local VkImage filled with the given ARGB color (0xAARRGGBB)
    [[nodiscard]] std::expected<VkImage, VulkanError> createSolidColorImage(uint32_t argb) noexcept;
    void releaseImage(VkImage image) noexcept;

    // Desktop wallpaper: decode the image at `path` (PNG/SVG) and upload it as the
    // background texture drawn below every window. Empty path clears it (falls back
    // to the solid clear colour). fit_mode: 0=Cover, 1=Contain, 2=Stretch, 3=Center
    // (matches WallpaperSetRequest). Safe to call live (single-threaded loop).
    void setWallpaper(const std::string& path, uint32_t fit_mode = 0) noexcept;

    // Per-output: allocate render + scanout images for output_idx
    std::expected<void, VulkanError> createTarget(size_t output_idx, uint32_t width, uint32_t height,
                                                   bool hdr = false) noexcept;
    // Backward-compat: creates target for output 0
    std::expected<void, VulkanError> createTarget(uint32_t width, uint32_t height) noexcept;

    // Returns the DRM pixel format for the scanout image of the given output.
    // DRM_FORMAT_XRGB2101010 (10-bit) when HDR active, DRM_FORMAT_XRGB8888 (8-bit) otherwise.
    [[nodiscard]] uint32_t getDrmFormat(size_t output_idx = 0) const noexcept;

    // Per-output: composite window stack into output_idx's framebuffer
    [[nodiscard]] std::expected<void, VulkanError> composite(
        size_t output_idx,
        const WindowStack& window_stack,
        float cursor_x = 0.0f, float cursor_y = 0.0f,
        float damage_x = 0.0f, float damage_y = 0.0f,
        float damage_width = 1920.0f, float damage_height = 1080.0f
    ) noexcept;
    // Backward-compat: composites into output 0
    [[nodiscard]] std::expected<void, VulkanError> composite(
        const WindowStack& window_stack,
        float cursor_x = 0.0f, float cursor_y = 0.0f,
        float damage_x = 0.0f, float damage_y = 0.0f,
        float damage_width = 1920.0f, float damage_height = 1080.0f
    ) noexcept;

    // Per-output: export the last composited frame as a DMABUF fd
    [[nodiscard]] std::expected<int, VulkanError> exportTarget(size_t output_idx) noexcept;
    // Backward-compat: exports output 0
    [[nodiscard]] std::expected<int, VulkanError> exportTarget() noexcept;

    // Reads back pixels from the target image to CPU memory (Testing only)
    [[nodiscard]] std::expected<std::vector<uint32_t>, VulkanError> readbackPixels(uint32_t width, uint32_t height) noexcept;
    [[nodiscard]] std::expected<std::vector<uint8_t>, VulkanError> inspectSurfacePixels(VkImage surface_image, uint32_t width, uint32_t height) noexcept;

    uint32_t getFrameChecksum() noexcept;
    [[nodiscard]] uint32_t getStride(size_t output_idx = 0) const noexcept;
    [[nodiscard]] uint32_t getCurrentFrame(size_t output_idx = 0) const noexcept;
    [[nodiscard]] size_t outputCount() const noexcept { return output_targets_.size(); }

    // Debug overlay (Super+F3): renders a GPU-timing HUD on output 0.
    void toggleDebugOverlay() noexcept { debug_overlay_visible_ = !debug_overlay_visible_; }
    [[nodiscard]] bool isDebugOverlayVisible() const noexcept { return debug_overlay_visible_; }

    // Per-output GPU profiler access (read-only, for external status queries).
    [[nodiscard]] const GpuProfiler* getProfiler(size_t output_idx = 0) const noexcept {
        if (output_idx >= gpu_profilers_.size()) return nullptr;
        return gpu_profilers_[output_idx].get();
    }

    // Destroys and removes output targets beyond `count` (called on monitor unplug)
    void trimOutputs(size_t count) noexcept;

    // Informs the decorator which surface currently has keyboard focus so it
    // can colour the traffic-light buttons appropriately (active vs. inactive).
    void setFocusedSurface(uint32_t surface_id) noexcept { focused_surface_id_ = surface_id; }

    // GPU cursor mode (iOS-style blob) — disabled by default.
    // Enable for touchpad/tablet mode; disable to use DRM hardware cursor.
    void setGpuCursorEnabled(bool enabled) noexcept;
    [[nodiscard]] bool isGpuCursorEnabled() const noexcept { return gpu_cursor_enabled_; }

    // iOS-style GPU cursor — call these from the render/input loops.
    // These are no-ops when GPU cursor is disabled.
    void updateCursorPosition(float x, float y) noexcept;
    void setCursorHoverTarget(float cx, float cy, float hw, float hh) noexcept;
    void clearCursorHoverTarget() noexcept;

    // Capture the current rendered frame for `output_idx` into an anonymous shm fd.
    // Returns (shm_fd, ScreencopyFrameInfo) on success; the caller owns the fd.
    [[nodiscard]] std::expected<std::pair<int, shared::ScreencopyFrameInfo>, VulkanError>
        captureToShm(size_t output_idx = 0) noexcept;

    [[nodiscard]] VkDevice getDevice() const noexcept { return device_; }
    [[nodiscard]] VkQueue getGraphicsQueue() const noexcept { return graphics_queue_; }

    bool needs_initial_clear_{true};

private:
    VulkanCompositor() noexcept = default;

    std::expected<void, VulkanError> init() noexcept;
    std::expected<void, VulkanError> setupPipeline() noexcept;
    std::expected<VkShaderModule, VulkanError> createShaderModule(const std::string& filename) const noexcept;
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const noexcept;
    void destroyOutputTarget(PerOutputTarget& ot) noexcept;

    VkInstance instance_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue graphics_queue_{VK_NULL_HANDLE};
    uint32_t graphics_queue_family_index_{0};

    VkRenderPass render_pass_{VK_NULL_HANDLE};
    VkRenderPass render_pass_load_{VK_NULL_HANDLE};
    VkDescriptorSetLayout descriptor_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_flat_{VK_NULL_HANDLE};
    VkPipeline graphics_pipeline_{VK_NULL_HANDLE};
    VkPipeline path_pipeline_{VK_NULL_HANDLE};
    VkPipeline solid_pipeline_{VK_NULL_HANDLE};
    VkPipeline rounded_pipeline_{VK_NULL_HANDLE};
    uint32_t   focused_surface_id_{999}; // surface with keyboard focus (999 = none)
    VkPipeline blur_h_pipeline_{VK_NULL_HANDLE};
    VkPipeline blur_v_pipeline_{VK_NULL_HANDLE};
    VkSampler sampler_{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
    struct SurfaceResource {
        VkDescriptorSet set_;
        VkImageView view_;
        VkDeviceMemory memory_;
    };
    std::unordered_map<VkImage, SurfaceResource> descriptor_sets_;

    // Per-output render + scanout targets
    std::vector<PerOutputTarget> output_targets_;

    // Per-output HDR mode flag (true = 10-bit HDR scanout image)
    std::vector<bool> output_hdr_mode_;

    // Offscreen Blur Resources (shared, output-independent)
    VkImage blur_temp_image_{VK_NULL_HANDLE};
    VkImageView blur_temp_view_{VK_NULL_HANDLE};
    VkDeviceMemory blur_temp_memory_{VK_NULL_HANDLE};
    VkFramebuffer blur_temp_framebuffer_{VK_NULL_HANDLE};
    VkDescriptorSet blur_temp_set_{VK_NULL_HANDLE};

    VkCommandPool command_pool_{VK_NULL_HANDLE};

    // Desktop wallpaper background texture (drawn below all windows).
    VkImage       wallpaper_image_{VK_NULL_HANDLE};
    std::string   wallpaper_path_;   // currently-loaded path (dedup live reloads)
    uint32_t      wallpaper_fit_{0}; // 0=Cover 1=Contain 2=Stretch 3=Center
    uint32_t      wallpaper_img_w_{0}, wallpaper_img_h_{0}; // decoded native size (for aspect)
    // Uploads an RGBA8 pixel buffer as a device-local sampled texture + descriptor set.
    [[nodiscard]] std::expected<VkImage, VulkanError>
        createImageFromPixels(const uint8_t* rgba, uint32_t w, uint32_t h) noexcept;

    std::unique_ptr<WindowDecorator> decorator_;

    // Title bar text rendering
    VkDescriptorSetLayout title_desc_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout      title_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline            title_pipeline_{VK_NULL_HANDLE};
    VkDescriptorPool      title_desc_pool_{VK_NULL_HANDLE};
    VkDescriptorSet       title_desc_set_{VK_NULL_HANDLE};
    VkSampler             title_sampler_{VK_NULL_HANDLE};
    VkImage               title_atlas_image_{VK_NULL_HANDLE};
    VkDeviceMemory        title_atlas_memory_{VK_NULL_HANDLE};
    VkImageView           title_atlas_view_{VK_NULL_HANDLE};
    std::unique_ptr<xrender::FontAtlas>  title_font_atlas_;
    std::unique_ptr<xrender::TextShaper> title_text_shaper_;

    void initTitleTextPipeline() noexcept;
    void renderDebugOverlay(VkCommandBuffer cmd, size_t output_idx) noexcept;

    // ── Pipeline cache ────────────────────────────────────────────────────────
    // Loaded from ~/.freebsdx-vk-cache at init; validated against physical-device
    // header (vendor/device ID + UUID). Saved back on destruction.
    VkPipelineCache pipeline_cache_{VK_NULL_HANDLE};
    void loadPipelineCache() noexcept;   // called after device creation
    void savePipelineCache() noexcept;   // called from destructor

    // ── Descriptor-set reuse pool ─────────────────────────────────────────────
    // Free-list of VkDescriptorSet handles; surfaces return their set here on
    // release instead of leaking it back into the pool. Pre-warmed with 64 sets
    // at init so the first window batch pays zero vkAllocateDescriptorSets cost.
    std::vector<VkDescriptorSet> ds_free_list_;
    VkDescriptorSet allocateDescriptorSet() noexcept;
    void            freeDescriptorSet(VkDescriptorSet set) noexcept;

    // Per-output GPU timestamp profilers (indexed by output_idx)
    std::vector<std::unique_ptr<GpuProfiler>> gpu_profilers_;
    bool debug_overlay_visible_{false};

    // VK_EXT_debug_utils function pointers (null when extension unavailable)
    PFN_vkCmdBeginDebugUtilsLabelEXT pfn_begin_label_{nullptr};
    PFN_vkCmdEndDebugUtilsLabelEXT   pfn_end_label_{nullptr};

    // Vertex Buffer for Paths (Dynamic, shared across outputs)
    VkBuffer vertex_buffer_{VK_NULL_HANDLE};
    VkDeviceMemory vertex_memory_{VK_NULL_HANDLE};
    void* vertex_mapped_{nullptr};

    // ── GPU cursor pipeline (iOS-style blob, for touchpad/tablet mode) ─────────
    bool gpu_cursor_enabled_{false};
    VkPipeline cursor_pipeline_{VK_NULL_HANDLE};
    GpuCursorState cursor_state_{};
    std::chrono::steady_clock::time_point cursor_last_frame_{};

    uint32_t tessellatePath(const shared::PathData& path, float* vertex_ptr) noexcept;
};

} // namespace fbx::xserver
