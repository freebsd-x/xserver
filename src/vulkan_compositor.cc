#include "xserver/vulkan_compositor.h"
#include "xserver/frame_perf_service.h"
#include "xserver/window_decorator.h"
#include "xserver/window_stack.h"
#include "xserver/notification_service.h"
#include "shared/protocol.h"
#include "shared/logger.h"
#include "xrender/font_atlas.h"
#include "xrender/text_shaper.h"
#include "xrender/font_manager.h"
#include "xrender/image_decoder.h"
#include <fontconfig/fontconfig.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <libdrm/drm_fourcc.h>
#include <chrono>

namespace fbx::xserver {

VulkanCompositor::VulkanCompositor(VulkanCompositor&& other) noexcept
    : instance_(std::exchange(other.instance_, VK_NULL_HANDLE)),
      physical_device_(std::exchange(other.physical_device_, VK_NULL_HANDLE)),
      device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      graphics_queue_(std::exchange(other.graphics_queue_, VK_NULL_HANDLE)),
      graphics_queue_family_index_(std::exchange(other.graphics_queue_family_index_, 0)),
      render_pass_(std::exchange(other.render_pass_, VK_NULL_HANDLE)),
      render_pass_load_(std::exchange(other.render_pass_load_, VK_NULL_HANDLE)),
      descriptor_set_layout_(std::exchange(other.descriptor_set_layout_, VK_NULL_HANDLE)),
      pipeline_layout_(std::exchange(other.pipeline_layout_, VK_NULL_HANDLE)),
      pipeline_layout_flat_(std::exchange(other.pipeline_layout_flat_, VK_NULL_HANDLE)),
      graphics_pipeline_(std::exchange(other.graphics_pipeline_, VK_NULL_HANDLE)),
      path_pipeline_(std::exchange(other.path_pipeline_, VK_NULL_HANDLE)),
      solid_pipeline_(std::exchange(other.solid_pipeline_, VK_NULL_HANDLE)),
      rounded_pipeline_(std::exchange(other.rounded_pipeline_, VK_NULL_HANDLE)),
      blur_h_pipeline_(std::exchange(other.blur_h_pipeline_, VK_NULL_HANDLE)),
      blur_v_pipeline_(std::exchange(other.blur_v_pipeline_, VK_NULL_HANDLE)),
      sampler_(std::exchange(other.sampler_, VK_NULL_HANDLE)),
      descriptor_pool_(std::exchange(other.descriptor_pool_, VK_NULL_HANDLE)),
      output_targets_(std::move(other.output_targets_)),
      command_pool_(std::exchange(other.command_pool_, VK_NULL_HANDLE)),
      vertex_buffer_(std::exchange(other.vertex_buffer_, VK_NULL_HANDLE)),
      vertex_memory_(std::exchange(other.vertex_memory_, VK_NULL_HANDLE)),
      vertex_mapped_(std::exchange(other.vertex_mapped_, nullptr)),
      cursor_pipeline_(std::exchange(other.cursor_pipeline_, VK_NULL_HANDLE)),
      cursor_state_(std::move(other.cursor_state_)) {}

VulkanCompositor& VulkanCompositor::operator=(VulkanCompositor&& other) noexcept {
    if (this != &other) {
        this->~VulkanCompositor();
        instance_ = std::exchange(other.instance_, VK_NULL_HANDLE);
        physical_device_ = std::exchange(other.physical_device_, VK_NULL_HANDLE);
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        graphics_queue_ = std::exchange(other.graphics_queue_, VK_NULL_HANDLE);
        graphics_queue_family_index_ = std::exchange(other.graphics_queue_family_index_, 0);
        render_pass_ = std::exchange(other.render_pass_, VK_NULL_HANDLE);
        render_pass_load_ = std::exchange(other.render_pass_load_, VK_NULL_HANDLE);
        descriptor_set_layout_ = std::exchange(other.descriptor_set_layout_, VK_NULL_HANDLE);
        pipeline_layout_ = std::exchange(other.pipeline_layout_, VK_NULL_HANDLE);
        pipeline_layout_flat_ = std::exchange(other.pipeline_layout_flat_, VK_NULL_HANDLE);
        graphics_pipeline_ = std::exchange(other.graphics_pipeline_, VK_NULL_HANDLE);
        path_pipeline_ = std::exchange(other.path_pipeline_, VK_NULL_HANDLE);
        solid_pipeline_ = std::exchange(other.solid_pipeline_, VK_NULL_HANDLE);
        rounded_pipeline_ = std::exchange(other.rounded_pipeline_, VK_NULL_HANDLE);
        blur_h_pipeline_ = std::exchange(other.blur_h_pipeline_, VK_NULL_HANDLE);
        blur_v_pipeline_ = std::exchange(other.blur_v_pipeline_, VK_NULL_HANDLE);
        descriptor_pool_ = std::exchange(other.descriptor_pool_, VK_NULL_HANDLE);
        sampler_ = std::exchange(other.sampler_, VK_NULL_HANDLE);
        output_targets_ = std::move(other.output_targets_);
        command_pool_ = std::exchange(other.command_pool_, VK_NULL_HANDLE);
        vertex_buffer_ = std::exchange(other.vertex_buffer_, VK_NULL_HANDLE);
        vertex_memory_ = std::exchange(other.vertex_memory_, VK_NULL_HANDLE);
        vertex_mapped_ = std::exchange(other.vertex_mapped_, nullptr);
        cursor_pipeline_ = std::exchange(other.cursor_pipeline_, VK_NULL_HANDLE);
        cursor_state_ = std::move(other.cursor_state_);
    }
    return *this;
}

void VulkanCompositor::destroyOutputTarget(PerOutputTarget& ot) noexcept {
    if (!device_) return;
    for (int i = 0; i < 2; ++i) {
        if (i < (int)ot.target_framebuffer.size() && ot.target_framebuffer[i])
            vkDestroyFramebuffer(device_, ot.target_framebuffer[i], nullptr);
        if (i < (int)ot.target_view.size() && ot.target_view[i])
            vkDestroyImageView(device_, ot.target_view[i], nullptr);
        if (i < (int)ot.target_image.size() && ot.target_image[i])
            vkDestroyImage(device_, ot.target_image[i], nullptr);
        if (i < (int)ot.target_memory.size() && ot.target_memory[i])
            vkFreeMemory(device_, ot.target_memory[i], nullptr);
        if (i < (int)ot.render_view.size() && ot.render_view[i])
            vkDestroyImageView(device_, ot.render_view[i], nullptr);
        if (i < (int)ot.render_image.size() && ot.render_image[i])
            vkDestroyImage(device_, ot.render_image[i], nullptr);
        if (i < (int)ot.render_memory.size() && ot.render_memory[i])
            vkFreeMemory(device_, ot.render_memory[i], nullptr);
    }
}

VulkanCompositor::~VulkanCompositor() noexcept {
    if (device_) {
        vkDeviceWaitIdle(device_);

        // Persist pipeline cache before destroying it.
        savePipelineCache();
        if (pipeline_cache_) vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);

        for (auto& pair : descriptor_sets_) {
            vkDestroyImageView(device_, pair.second.view_, nullptr);
            vkFreeMemory(device_, pair.second.memory_, nullptr);
            vkDestroyImage(device_, pair.first, nullptr);
            // Descriptor sets are freed implicitly when the pool is destroyed below.
        }
        // Drain the reuse free-list (also freed by pool destruction, but clear to be tidy).
        ds_free_list_.clear();
        if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
        if (vertex_mapped_) vkUnmapMemory(device_, vertex_memory_);
        if (vertex_buffer_) vkDestroyBuffer(device_, vertex_buffer_, nullptr);
        if (vertex_memory_) vkFreeMemory(device_, vertex_memory_, nullptr);
        if (graphics_pipeline_) vkDestroyPipeline(device_, graphics_pipeline_, nullptr);
        if (path_pipeline_) vkDestroyPipeline(device_, path_pipeline_, nullptr);
        if (solid_pipeline_) vkDestroyPipeline(device_, solid_pipeline_, nullptr);
        if (rounded_pipeline_) vkDestroyPipeline(device_, rounded_pipeline_, nullptr);
        if (blur_h_pipeline_) vkDestroyPipeline(device_, blur_h_pipeline_, nullptr);
        if (blur_v_pipeline_) vkDestroyPipeline(device_, blur_v_pipeline_, nullptr);
        if (cursor_pipeline_) vkDestroyPipeline(device_, cursor_pipeline_, nullptr);
        if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        if (pipeline_layout_flat_) vkDestroyPipelineLayout(device_, pipeline_layout_flat_, nullptr);
        if (descriptor_set_layout_) vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
        if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
        if (render_pass_load_) vkDestroyRenderPass(device_, render_pass_load_, nullptr);

        for (auto& ot : output_targets_) destroyOutputTarget(ot);

        // Destroy GpuProfiler objects before vkDestroyDevice: each profiler
        // holds a VkQueryPool that must be freed while the device is still valid.
        // Member destructors run after the destructor body, which would be too late.
        gpu_profilers_.clear();

        if (title_pipeline_) vkDestroyPipeline(device_, title_pipeline_, nullptr);
        if (title_pipeline_layout_) vkDestroyPipelineLayout(device_, title_pipeline_layout_, nullptr);
        if (title_desc_set_layout_) vkDestroyDescriptorSetLayout(device_, title_desc_set_layout_, nullptr);
        if (title_desc_pool_) vkDestroyDescriptorPool(device_, title_desc_pool_, nullptr);
        if (title_sampler_) vkDestroySampler(device_, title_sampler_, nullptr);
        if (title_atlas_view_) vkDestroyImageView(device_, title_atlas_view_, nullptr);
        if (title_atlas_image_) vkDestroyImage(device_, title_atlas_image_, nullptr);
        if (title_atlas_memory_) vkFreeMemory(device_, title_atlas_memory_, nullptr);
        if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

std::expected<std::unique_ptr<VulkanCompositor>, VulkanError> VulkanCompositor::create() noexcept {
    auto comp = std::unique_ptr<VulkanCompositor>(new VulkanCompositor());
    if (auto res = comp->init(); !res) return std::unexpected(res.error());
    return comp;
}

std::expected<void, VulkanError> VulkanCompositor::init() noexcept {
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO,0,"freebsdx",1,"freebsdx-engine",1,VK_API_VERSION_1_3};

    // Probe for VK_EXT_debug_utils (optional — benefits GPU profilers like RenderDoc).
    bool has_debug_utils = false;
    {
        uint32_t ec = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &ec, nullptr);
        std::vector<VkExtensionProperties> avail(ec);
        vkEnumerateInstanceExtensionProperties(nullptr, &ec, avail.data());
        for (const auto& e : avail)
            if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
                { has_debug_utils = true; break; }
    }

    std::vector<const char*> inst_exts = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    if (has_debug_utils) inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,0,0,&ai,0,0,
        static_cast<uint32_t>(inst_exts.size()), inst_exts.data()};
    if (vkCreateInstance(&ici, 0, &instance_) != VK_SUCCESS) return std::unexpected(VulkanError::InstanceCreationFailed);

    // Load VK_EXT_debug_utils function pointers if the extension was enabled.
    if (has_debug_utils) {
        pfn_begin_label_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(instance_, "vkCmdBeginDebugUtilsLabelEXT"));
        pfn_end_label_ = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(instance_, "vkCmdEndDebugUtilsLabelEXT"));
        if (pfn_begin_label_)
            std::cout << "[VulkanCompositor] VK_EXT_debug_utils enabled — render-pass labels active\n";
    }

    uint32_t dc = 0; vkEnumeratePhysicalDevices(instance_, &dc, nullptr);
    std::vector<VkPhysicalDevice> devices(dc); vkEnumeratePhysicalDevices(instance_, &dc, devices.data());
    physical_device_ = devices[0];

    uint32_t qfc = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qfc, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qfc); vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qfc, qf.data());
    int qfi = -1; for(int i=0; i<(int)qfc; ++i) if(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
    graphics_queue_family_index_ = qfi;

    float qp = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,0,0,graphics_queue_family_index_,1,&qp};
    const char* dext[] = { VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, "VK_KHR_external_memory_fd" };
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,0,0,1,&qci,0,0,4,dext,0};
    if (vkCreateDevice(physical_device_, &dci, 0, &device_) != VK_SUCCESS) return std::unexpected(VulkanError::LogicalDeviceCreationFailed);
    vkGetDeviceQueue(device_, graphics_queue_family_index_, 0, &graphics_queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,0,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,graphics_queue_family_index_};
    vkCreateCommandPool(device_, &pci, 0, &command_pool_);

    // ── Sampler created first so setupPipeline() can bake it as immutable ────
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,0,0,VK_FILTER_LINEAR,VK_FILTER_LINEAR,VK_SAMPLER_MIPMAP_MODE_LINEAR,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,0,0,0,0,VK_COMPARE_OP_NEVER,0,0,VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,0};
    vkCreateSampler(device_, &si, 0, &sampler_);

    // ── Pipeline cache: load persisted data from disk ─────────────────────────
    loadPipelineCache();

    if (auto res = setupPipeline(); !res) return res;

    // ── Descriptor pool — FREE_DESCRIPTOR_SET_BIT enables per-set freeing ────
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0,
        VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, 1024, 1, &ps};
    vkCreateDescriptorPool(device_, &dpci, 0, &descriptor_pool_);

    // ── Pre-warm descriptor-set reuse pool (64 sets) ──────────────────────────
    // Subsequent surface imports draw from this list before calling
    // vkAllocateDescriptorSets, keeping the hot path allocation-free.
    ds_free_list_.reserve(64);
    for (int i = 0; i < 64; ++i) {
        VkDescriptorSetAllocateInfo pw_dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            nullptr, descriptor_pool_, 1, &descriptor_set_layout_};
        VkDescriptorSet s = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device_, &pw_dsai, &s) == VK_SUCCESS)
            ds_free_list_.push_back(s);
    }
    std::cout << "[VulkanCompositor] Descriptor-set pool pre-warmed with "
              << ds_free_list_.size() << " sets\n";

    VkBufferCreateInfo vbi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,0,0,1024*1024,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,VK_SHARING_MODE_EXCLUSIVE,0,0};
    vkCreateBuffer(device_, &vbi, 0, &vertex_buffer_);
    VkMemoryRequirements vmr; vkGetBufferMemoryRequirements(device_, vertex_buffer_, &vmr);
    VkMemoryAllocateInfo vai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,0,vmr.size,findMemoryType(vmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    vkAllocateMemory(device_, &vai, 0, &vertex_memory_);
    vkBindBufferMemory(device_, vertex_buffer_, vertex_memory_, 0);
    vkMapMemory(device_, vertex_memory_, 0, 1024*1024, 0, &vertex_mapped_);

    // Initialize GPU cursor spring state to screen center
    cursor_state_.sx.pos = cursor_state_.sx.target = 960.f;
    cursor_state_.sy.pos = cursor_state_.sy.target = 540.f;
    cursor_state_.sw.pos = cursor_state_.sw.target = 11.f;  // default: 22px diameter
    cursor_state_.sh.pos = cursor_state_.sh.target = 11.f;
    cursor_state_.sr.pos = cursor_state_.sr.target = 11.f;  // full circle
    cursor_state_.salpha.pos = cursor_state_.salpha.target = 0.72f;
    cursor_last_frame_ = std::chrono::steady_clock::now();

    // Create primary output target (output 0) at default resolution
    if (auto res = createTarget(0, 1920, 1080); !res) return res;

    decorator_ = std::make_unique<WindowDecorator>(
        device_, render_pass_, solid_pipeline_, pipeline_layout_flat_,
        output_targets_[0].width, output_targets_[0].height);
    decorator_->setRoundedPipeline(rounded_pipeline_);

    initTitleTextPipeline();

    return {};
}

std::expected<void, VulkanError> VulkanCompositor::setupPipeline() noexcept {
    VkAttachmentDescription ca{0, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference cr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 1, &cr, 0, 0, 0, 0};
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,0,0,1,&ca,1,&sub,0,0};
    vkCreateRenderPass(device_, &rpi, 0, &render_pass_);

    VkAttachmentDescription ca_load = ca; ca_load.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca_load.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkRenderPassCreateInfo rpi_load{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,0,0,1,&ca_load,1,&sub,0,0};
    vkCreateRenderPass(device_, &rpi_load, 0, &render_pass_load_);

    // Immutable sampler baked into the layout: vkUpdateDescriptorSets for surface
    // descriptors only needs to write the image-view portion, not the sampler.
    // sampler_ is valid here because it is created before setupPipeline() in init().
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
        VK_SHADER_STAGE_FRAGMENT_BIT, &sampler_};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,0,0,1,&b};
    vkCreateDescriptorSetLayout(device_, &lci, 0, &descriptor_set_layout_);
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 40};
    VkPushConstantRange pcr_flat{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,0,0,1,&descriptor_set_layout_,1,&pcr};
    vkCreatePipelineLayout(device_, &pli, 0, &pipeline_layout_);

    VkPipelineLayoutCreateInfo pli_flat{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,0,0,0,nullptr,1,&pcr_flat};
    vkCreatePipelineLayout(device_, &pli_flat, 0, &pipeline_layout_flat_);

    auto createPipe = [&](const char* v, const char* f, VkPrimitiveTopology t, bool vi, VkPipelineLayout l) -> VkPipeline {
        auto vm = createShaderModule(v); auto fm = createShaderModule(f);
        if (!vm || !fm) return VK_NULL_HANDLE;
        VkPipelineShaderStageCreateInfo ss[] = { {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,0,0,VK_SHADER_STAGE_VERTEX_BIT,vm.value(),"main",0}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,0,0,VK_SHADER_STAGE_FRAGMENT_BIT,fm.value(),"main",0} };
        VkVertexInputBindingDescription bind{0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attr{0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
        VkPipelineVertexInputStateCreateInfo vis{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,0,0,0,0,0,0};
        if (vi) { vis.vertexBindingDescriptionCount = 1; vis.pVertexBindingDescriptions = &bind; vis.vertexAttributeDescriptionCount = 1; vis.pVertexAttributeDescriptions = &attr; }
        VkPipelineInputAssemblyStateCreateInfo ias{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,0,0,t,0};
        VkViewport vp{0,0,1920,1080,0,1}; VkRect2D sc{{0,0},{1920,1080}};
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,0,0,1,&vp,1,&sc};
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,0,0,0,0,VK_POLYGON_MODE_FILL,VK_CULL_MODE_NONE,VK_FRONT_FACE_CLOCKWISE,0,0,0,0,1};
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,0,0,VK_SAMPLE_COUNT_1_BIT,0,1,0,0,0};
        VkPipelineColorBlendAttachmentState ba{1, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xf};
        VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,0,0,0,VK_LOGIC_OP_COPY,1,&ba,{0,0,0,0}};
        VkDynamicState ds[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR }; VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,0,0,2,ds};
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,0,0,2,ss,&vis,&ias,0,&vps,&rs,&ms,0,&bs,&dsi,l,render_pass_,0,0,0};
        VkPipeline p; vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &pi, 0, &p);
        vkDestroyShaderModule(device_, vm.value(), 0); vkDestroyShaderModule(device_, fm.value(), 0);
        return p;
    };

    graphics_pipeline_ = createPipe("freebsdx.p/blit.vert.spv", "freebsdx.p/blit.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, false, pipeline_layout_);
    path_pipeline_ = createPipe("freebsdx.p/path.vert.spv", "freebsdx.p/path.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, pipeline_layout_flat_);
    solid_pipeline_ = createPipe("freebsdx.p/solid.vert.spv", "freebsdx.p/solid.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, false, pipeline_layout_flat_);
    rounded_pipeline_ = createPipe("freebsdx.p/rounded.vert.spv", "freebsdx.p/rounded.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, false, pipeline_layout_flat_);
    cursor_pipeline_ = createPipe("freebsdx.p/cursor.vert.spv", "freebsdx.p/cursor.frag.spv",
                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, false, pipeline_layout_flat_);
    return {};
}

std::expected<void, VulkanError> VulkanCompositor::createTarget(size_t output_idx, uint32_t w, uint32_t h, bool hdr) noexcept {
    // Extend vector if needed
    if (output_idx >= output_targets_.size()) {
        output_targets_.resize(output_idx + 1);
    }
    auto& ot = output_targets_[output_idx];

    // Destroy any existing resources for this slot
    destroyOutputTarget(ot);
    ot = PerOutputTarget{};
    ot.width = w; ot.height = h;

    // Store HDR mode for this output (used by getDrmFormat()).
    if (output_idx >= output_hdr_mode_.size())
        output_hdr_mode_.resize(output_idx + 1, false);
    output_hdr_mode_[output_idx] = hdr;

    // render_fmt: always 8-bit BGRA for render pass compatibility.
    // target_fmt: 10-bit A2R10G10B10 for HDR scanout, 8-bit BGRA for SDR.
    const VkFormat render_fmt = VK_FORMAT_B8G8R8A8_UNORM;
    const VkFormat target_fmt = hdr ? VK_FORMAT_A2R10G10B10_UNORM_PACK32
                                    : VK_FORMAT_B8G8R8A8_UNORM;

    VkImageCreateInfo ri{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,0,0,VK_IMAGE_TYPE_2D,render_fmt,{w,h,1},1,1,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_SHARING_MODE_EXCLUSIVE,0,0,VK_IMAGE_LAYOUT_UNDEFINED};
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,0,0,VK_IMAGE_TYPE_2D,target_fmt,{w,h,1},1,1,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_TILING_LINEAR,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_SHARING_MODE_EXCLUSIVE,0,0,VK_IMAGE_LAYOUT_UNDEFINED};
    VkExportMemoryAllocateInfo ex{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,0,VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,&ex,0,0};
    VkComponentMapping ident = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

    XASSERT(device_ != VK_NULL_HANDLE, "VulkanCompositor::createTarget called before init()");
    XASSERT(render_pass_ != VK_NULL_HANDLE, "render_pass_ must exist before createTarget()");

    for (int i = 0; i < 2; ++i) {
        // Optimal Render Target (always 8-bit for render pass compatibility)
        if (vkCreateImage(device_, &ri, 0, &ot.render_image[i]) != VK_SUCCESS) {
            std::cerr << "[VulkanCompositor] createTarget: vkCreateImage (render[" << i << "]) failed\n";
            destroyOutputTarget(ot); return std::unexpected(VulkanError::MemoryAllocationFailed);
        }
        VkMemoryRequirements rmr; vkGetImageMemoryRequirements(device_, ot.render_image[i], &rmr);
        VkMemoryAllocateInfo rai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,0,rmr.size,findMemoryType(rmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        if (vkAllocateMemory(device_, &rai, 0, &ot.render_memory[i]) != VK_SUCCESS) {
            std::cerr << "[VulkanCompositor] createTarget: vkAllocateMemory (render[" << i << "]) failed\n";
            destroyOutputTarget(ot); return std::unexpected(VulkanError::MemoryAllocationFailed);
        }
        vkBindImageMemory(device_, ot.render_image[i], ot.render_memory[i], 0);
        VkImageViewCreateInfo rvi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,0,0,ot.render_image[i],VK_IMAGE_VIEW_TYPE_2D,render_fmt,ident,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        if (vkCreateImageView(device_, &rvi, 0, &ot.render_view[i]) != VK_SUCCESS) {
            std::cerr << "[VulkanCompositor] createTarget: vkCreateImageView (render[" << i << "]) failed\n";
            destroyOutputTarget(ot); return std::unexpected(VulkanError::MemoryAllocationFailed);
        }

        // Linear Scanout Target (10-bit for HDR, 8-bit for SDR)
        if (vkCreateImage(device_, &ii, 0, &ot.target_image[i]) != VK_SUCCESS) {
            std::cerr << "[VulkanCompositor] createTarget: vkCreateImage (target[" << i << "]) failed\n";
            destroyOutputTarget(ot); return std::unexpected(VulkanError::MemoryAllocationFailed);
        }
        VkMemoryRequirements tmr; vkGetImageMemoryRequirements(device_, ot.target_image[i], &tmr);
        ai.allocationSize = tmr.size; ai.memoryTypeIndex = findMemoryType(tmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device_, &ai, 0, &ot.target_memory[i]) != VK_SUCCESS) {
            std::cerr << "[VulkanCompositor] createTarget: vkAllocateMemory (target[" << i << "]) failed\n";
            destroyOutputTarget(ot); return std::unexpected(VulkanError::MemoryAllocationFailed);
        }
        vkBindImageMemory(device_, ot.target_image[i], ot.target_memory[i], 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,0,0,ot.target_image[i],VK_IMAGE_VIEW_TYPE_2D,target_fmt,ident,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        if (vkCreateImageView(device_, &vi, 0, &ot.target_view[i]) != VK_SUCCESS) {
            std::cerr << "[VulkanCompositor] createTarget: vkCreateImageView (target[" << i << "]) failed\n";
            destroyOutputTarget(ot); return std::unexpected(VulkanError::MemoryAllocationFailed);
        }

        // Framebuffer — render into the optimal image
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,0,0,render_pass_,1,&ot.render_view[i],w,h,1};
        if (vkCreateFramebuffer(device_, &fi, 0, &ot.target_framebuffer[i]) != VK_SUCCESS) {
            std::cerr << "[VulkanCompositor] createTarget: vkCreateFramebuffer[" << i << "] failed\n";
            destroyOutputTarget(ot); return std::unexpected(VulkanError::MemoryAllocationFailed);
        }
    }

    // Query stride from the linear scanout image
    VkImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device_, ot.target_image[0], &subresource, &layout);
    ot.stride = static_cast<uint32_t>(layout.rowPitch);

    // Transition all images to VK_IMAGE_LAYOUT_GENERAL
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,0,command_pool_,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
    VkCommandBuffer cmd; vkAllocateCommandBuffers(device_, &cai, &cmd);
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,0,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,0};
    vkBeginCommandBuffer(cmd, &cbi);

    VkImageMemoryBarrier barriers[4] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,0,0,0,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,ot.render_image[0],{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,0,0,0,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,ot.target_image[0],{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,0,0,0,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,ot.render_image[1],{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,0,0,0,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,ot.target_image[1],{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}}
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 4, barriers);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO,0,0,0,0,1,&cmd,0,0};
    vkQueueSubmit(graphics_queue_, 1, &si, 0); vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);

    std::cout << "[xserver] Output " << output_idx << " target created: "
              << w << "x" << h << " stride=" << ot.stride << "\n";

    // Create / replace the per-output GPU profiler.
    if (output_idx >= gpu_profilers_.size())
        gpu_profilers_.resize(output_idx + 1);
    gpu_profilers_[output_idx] = std::make_unique<GpuProfiler>();
    if (!gpu_profilers_[output_idx]->init(device_, physical_device_)) {
        // Non-fatal: profiler simply won't record if init fails.
        gpu_profilers_[output_idx].reset();
    }

    return {};
}

// Backward-compat wrapper
std::expected<void, VulkanError> VulkanCompositor::createTarget(uint32_t w, uint32_t h) noexcept {
    return createTarget(0, w, h, false);
}

uint32_t VulkanCompositor::getDrmFormat(size_t output_idx) const noexcept {
    if (output_idx < output_hdr_mode_.size() && output_hdr_mode_[output_idx])
        return DRM_FORMAT_XRGB2101010; // 10-bit HDR
    return DRM_FORMAT_XRGB8888;        // 8-bit SDR
}

std::expected<VkImage, VulkanError> VulkanCompositor::importExternalBuffer(int fd, uint32_t w, uint32_t h) noexcept {
    VkExternalMemoryImageCreateInfo ei{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,0,VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,&ei,0,VK_IMAGE_TYPE_2D,VK_FORMAT_B8G8R8A8_UNORM,{w,h,1},1,1,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_SAMPLED_BIT,VK_SHARING_MODE_EXCLUSIVE,0,0,VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img; if (vkCreateImage(device_, &ii, 0, &img) != VK_SUCCESS) return std::unexpected(VulkanError::ImportFailed);
    VkMemoryRequirements imr; vkGetImageMemoryRequirements(device_, img, &imr);
    VkImportMemoryFdInfoKHR im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,0,VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,fd};
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,&im,imr.size,findMemoryType(imr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory mem; if (vkAllocateMemory(device_, &ai, 0, &mem) != VK_SUCCESS) { vkDestroyImage(device_, img, 0); return std::unexpected(VulkanError::MemoryAllocationFailed); }
    vkBindImageMemory(device_, img, mem, 0);
    VkComponentMapping ident = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,0,0,img,VK_IMAGE_VIEW_TYPE_2D,VK_FORMAT_B8G8R8A8_UNORM,ident,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView view; vkCreateImageView(device_, &vi, 0, &view);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,0,command_pool_,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
    VkCommandBuffer cmd; vkAllocateCommandBuffers(device_, &cai, &cmd);
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,0,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,0};
    vkBeginCommandBuffer(cmd, &cbi);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,0,0,VK_ACCESS_SHADER_READ_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,img,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, 0, 0, 0, 1, &barrier);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO,0,0,0,0,1,&cmd,0,0};
    vkQueueSubmit(graphics_queue_, 1, &si, 0); vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);

    // Reuse a descriptor set from the pool; sampler is immutable — only write image view.
    VkDescriptorSet set = allocateDescriptorSet();
    VkDescriptorImageInfo dii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,0,set,0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&dii,0,0};
    vkUpdateDescriptorSets(device_, 1, &ws, 0, 0);
    descriptor_sets_[img] = {set, view, mem}; return img;
}

std::expected<VkImage, VulkanError> VulkanCompositor::createSolidColorImage(uint32_t argb) noexcept {
    uint8_t a = (argb >> 24) & 0xFF;
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >>  8) & 0xFF;
    uint8_t b = (argb      ) & 0xFF;

    // Device-local 1×1 image for sampling
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr, 0,
        VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, {1, 1, 1}, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vkCreateImage(device_, &ii, nullptr, &img) != VK_SUCCESS)
        return std::unexpected(VulkanError::ImportFailed);

    VkMemoryRequirements imr;
    vkGetImageMemoryRequirements(device_, img, &imr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
        imr.size, findMemoryType(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory mem;
    if (vkAllocateMemory(device_, &mai, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyImage(device_, img, nullptr);
        return std::unexpected(VulkanError::MemoryAllocationFailed);
    }
    vkBindImageMemory(device_, img, mem, 0);

    // Staging buffer: 4 bytes [B, G, R, A] for B8G8R8A8_UNORM
    uint8_t pixel[4] = {b, g, r, a};
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0,
        4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
    VkBuffer staging;
    if (vkCreateBuffer(device_, &bci, nullptr, &staging) != VK_SUCCESS) {
        vkFreeMemory(device_, mem, nullptr);
        vkDestroyImage(device_, img, nullptr);
        return std::unexpected(VulkanError::ImportFailed);
    }
    VkMemoryRequirements smr;
    vkGetBufferMemoryRequirements(device_, staging, &smr);
    VkMemoryAllocateInfo sai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
        smr.size, findMemoryType(smr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory smem;
    if (vkAllocateMemory(device_, &sai, nullptr, &smem) != VK_SUCCESS) {
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, mem, nullptr);
        vkDestroyImage(device_, img, nullptr);
        return std::unexpected(VulkanError::MemoryAllocationFailed);
    }
    vkBindBufferMemory(device_, staging, smem, 0);
    void* mapped;
    vkMapMemory(device_, smem, 0, 4, 0, &mapped);
    std::memcpy(mapped, pixel, 4);
    vkUnmapMemory(device_, smem);

    // Record commands: UNDEFINED → TRANSFER_DST → copy → GENERAL
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        nullptr, command_pool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &cai, &cmd);
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    vkBeginCommandBuffer(cmd, &cbi);

    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);

    VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}};
    vkCmdCopyBufferToImage(cmd, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_general{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_general);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd, 0, nullptr};
    vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, smem, nullptr);

    // Image view + descriptor set (same pattern as importExternalBuffer)
    VkComponentMapping ident = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0,
        img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, ident,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    vkCreateImageView(device_, &vi, nullptr, &view);

    // Reuse a descriptor set from the pool; sampler is immutable — only write image view.
    VkDescriptorSet set = allocateDescriptorSet();
    VkDescriptorImageInfo dii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii, nullptr, nullptr};
    vkUpdateDescriptorSets(device_, 1, &ws, 0, nullptr);

    descriptor_sets_[img] = {set, view, mem};
    return img;
}

// Upload a w×h RGBA8 pixel buffer as a device-local sampled texture. Mirrors
// createSolidColorImage but copies a full image. The ImageDecoder produces RGBA
// bytes, so the image is R8G8B8A8_UNORM — the sampler returns (R,G,B,A) either
// way, so no channel swizzle is needed vs the compositor's B8G8R8A8 surfaces.
std::expected<VkImage, VulkanError>
VulkanCompositor::createImageFromPixels(const uint8_t* rgba, uint32_t w, uint32_t h) noexcept {
    if (!rgba || w == 0 || h == 0) return std::unexpected(VulkanError::ImportFailed);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr, 0,
        VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {w, h, 1}, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vkCreateImage(device_, &ii, nullptr, &img) != VK_SUCCESS)
        return std::unexpected(VulkanError::ImportFailed);

    VkMemoryRequirements imr;
    vkGetImageMemoryRequirements(device_, img, &imr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
        imr.size, findMemoryType(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory mem;
    if (vkAllocateMemory(device_, &mai, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyImage(device_, img, nullptr);
        return std::unexpected(VulkanError::MemoryAllocationFailed);
    }
    vkBindImageMemory(device_, img, mem, 0);

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0,
        bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
    VkBuffer staging;
    if (vkCreateBuffer(device_, &bci, nullptr, &staging) != VK_SUCCESS) {
        vkFreeMemory(device_, mem, nullptr);
        vkDestroyImage(device_, img, nullptr);
        return std::unexpected(VulkanError::ImportFailed);
    }
    VkMemoryRequirements smr;
    vkGetBufferMemoryRequirements(device_, staging, &smr);
    VkMemoryAllocateInfo sai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
        smr.size, findMemoryType(smr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory smem;
    if (vkAllocateMemory(device_, &sai, nullptr, &smem) != VK_SUCCESS) {
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, mem, nullptr);
        vkDestroyImage(device_, img, nullptr);
        return std::unexpected(VulkanError::MemoryAllocationFailed);
    }
    vkBindBufferMemory(device_, staging, smem, 0);
    void* mapped;
    vkMapMemory(device_, smem, 0, bytes, 0, &mapped);
    std::memcpy(mapped, rgba, bytes);
    vkUnmapMemory(device_, smem);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        nullptr, command_pool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &cai, &cmd);
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    vkBeginCommandBuffer(cmd, &cbi);

    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);

    VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyBufferToImage(cmd, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_general{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_general);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd, 0, nullptr};
    vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, smem, nullptr);

    VkComponentMapping ident = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0,
        img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, ident,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    vkCreateImageView(device_, &vi, nullptr, &view);

    VkDescriptorSet set = allocateDescriptorSet();
    VkDescriptorImageInfo dii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii, nullptr, nullptr};
    vkUpdateDescriptorSets(device_, 1, &ws, 0, nullptr);

    descriptor_sets_[img] = {set, view, mem};
    return img;
}

void VulkanCompositor::setWallpaper(const std::string& path, uint32_t fit_mode) noexcept {
    // A fit-only change needs no re-decode — just restamp and let composite() redraw.
    if (path == wallpaper_path_ && !path.empty()) { wallpaper_fit_ = fit_mode; return; }
    // Clear request: drop the texture, fall back to the solid clear colour.
    if (path.empty()) {
        if (wallpaper_image_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            releaseImage(wallpaper_image_);
            wallpaper_image_ = VK_NULL_HANDLE;
        }
        wallpaper_path_.clear();
        wallpaper_img_w_ = wallpaper_img_h_ = 0;
        return;
    }
    wallpaper_fit_ = fit_mode;

    // Decode at 1080p; the full-screen quad stretches it to the output. SVGs scale
    // cleanly; raster images are resampled by the decoder.
    auto decoded = fbx::xrender::ImageDecoder::decode(path, 1920, 1080);
    if (!decoded) {
        std::cerr << "[Wallpaper] decode failed: " << path << "\n";
        return;
    }
    auto img = createImageFromPixels(decoded->pixels_.data(),
                                     decoded->width_, decoded->height_);
    if (!img) {
        std::cerr << "[Wallpaper] GPU upload failed: " << path << "\n";
        return;
    }
    // Swap in the new texture, then release the old (GPU idled by the upload submit).
    VkImage old = wallpaper_image_;
    wallpaper_image_ = img.value();
    wallpaper_path_ = path;
    wallpaper_img_w_ = decoded->width_;
    wallpaper_img_h_ = decoded->height_;
    if (old != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        releaseImage(old);
    }
    std::cerr << "[Wallpaper] loaded " << path << " ("
              << decoded->width_ << "x" << decoded->height_ << ")\n";
}

std::expected<void, VulkanError> VulkanCompositor::composite(
    size_t output_idx,
    const WindowStack& ws,
    float cx, float cy,
    float dx, float dy, float dw, float dh) noexcept
{
    (void)dx; (void)dy; (void)dw; (void)dh;
    if (output_idx >= output_targets_.size())
        return std::unexpected(VulkanError::ImportFailed);
    auto& ot = output_targets_[output_idx];

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,0,command_pool_,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
    VkCommandBuffer cmd; vkAllocateCommandBuffers(device_, &ai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,0,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,0};
    vkBeginCommandBuffer(cmd, &bi);

    // GPU timestamp: frame start (top-of-pipe).
    GpuProfiler* profiler = (output_idx < gpu_profilers_.size())
                          ? gpu_profilers_[output_idx].get() : nullptr;
    if (profiler) profiler->beginFrame(cmd);

    // VK_EXT_debug_utils label: lets RenderDoc / NSight name this region.
    if (pfn_begin_label_) {
        char label_name[64];
        std::snprintf(label_name, sizeof(label_name), "Composite output[%zu]", output_idx);
        VkDebugUtilsLabelEXT lbl{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, nullptr,
            label_name, {0.2f, 0.6f, 1.0f, 1.0f}};
        pfn_begin_label_(cmd, &lbl);
    }

    uint32_t buf_idx = ot.current_frame % 2;
    // Desktop background. The shell renders a transparent desktop and sits above
    // app windows, so this clear colour is the wallpaper shown behind every app.
    VkClearValue cv;
    cv.color.float32[0] = 0.04f; cv.color.float32[1] = 0.10f; cv.color.float32[2] = 0.18f; cv.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rpb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,0,render_pass_,ot.target_framebuffer[buf_idx],{{0,0},{ot.width,ot.height}},1,&cv};
    vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0, 0, (float)ot.width, (float)ot.height, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor = {{0,0},{ot.width,ot.height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // HiDPI: window positions are in logical pixels; convert to physical via scale.
    // Passing (physical / scale) as the NDC normalizer makes the shader compute
    //   ndc = (logical_coord * scale / physical_dim) * 2 - 1
    // which correctly maps logical coordinates to physical pixel positions.
    float scale = ws.getOutputScale(output_idx);
    float logical_w = static_cast<float>(ot.width)  / scale;
    float logical_h = static_cast<float>(ot.height) / scale;

    // Desktop wallpaper: background texture drawn below every window. The quad is
    // sized per fit_mode (overflow is clipped by the scissor; letterboxed areas keep
    // the clear colour). When unset, the render-pass clear colour is the background.
    if (wallpaper_image_ != VK_NULL_HANDLE && graphics_pipeline_ != VK_NULL_HANDLE) {
        auto wit = descriptor_sets_.find(wallpaper_image_);
        if (wit != descriptor_sets_.end()) {
            // Default (Stretch): fill the whole screen, ignoring aspect.
            float draw_w = logical_w, draw_h = logical_h, off_x = 0.0f, off_y = 0.0f;
            const float iw = static_cast<float>(wallpaper_img_w_);
            const float ih = static_cast<float>(wallpaper_img_h_);
            if (iw > 0.0f && ih > 0.0f) {
                switch (wallpaper_fit_) {
                    case 0: {  // Cover: scale to fill, crop overflow (aspect preserved)
                        float s = std::max(logical_w / iw, logical_h / ih);
                        draw_w = iw * s; draw_h = ih * s;
                        break;
                    }
                    case 1: {  // Contain: scale to fit entirely, letterbox (aspect preserved)
                        float s = std::min(logical_w / iw, logical_h / ih);
                        draw_w = iw * s; draw_h = ih * s;
                        break;
                    }
                    case 3:    // Center: native size (1 image px → 1 logical px), no scale
                        draw_w = iw; draw_h = ih;
                        break;
                    case 2:    // Stretch: handled by the defaults above
                    default:
                        break;
                }
                off_x = (logical_w - draw_w) * 0.5f;
                off_y = (logical_h - draw_h) * 0.5f;
            }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipeline_layout_, 0, 1, &wit->second.set_, 0, 0);
            float wpc[10] = { off_x, off_y, draw_w, draw_h, logical_w, logical_h,
                              1.0f, 1.0f, 0.0f, 0.0f };
            vkCmdPushConstants(cmd, pipeline_layout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, 40, wpc);
            vkCmdDraw(cmd, 4, 1, 0, 0);
        }
    }

    // Use per-output filtered + coordinate-adjusted window list
    auto windows = ws.getWindowsForOutput(output_idx);
    for (const auto& w : windows) {
        if (w.surface_image_ == VK_NULL_HANDLE) continue;
        auto it = descriptor_sets_.find(w.surface_image_);
        if (it != descriptor_sets_.end() && graphics_pipeline_ != VK_NULL_HANDLE) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &it->second.set_, 0, 0);
            // When window geometry is set the buffer is larger than the content
            // area (includes CSD shadows). Shift the render origin so the geometry
            // rect aligns with position_; shadow pixels extend outside naturally.
            const bool has_geom = (w.geom_w_ > 0);
            float render_x = w.position_.x_ - (has_geom ? static_cast<float>(w.geom_x_) : 0.0f);
            float render_y = w.position_.y_ - (has_geom ? static_cast<float>(w.geom_y_) : 0.0f);
            float pc[10] = { render_x, render_y, w.size_.width_, w.size_.height_,
                             logical_w, logical_h,
                             w.scale_, w.opacity_, (w.is_shell_?0:ws.scene_offset_), 0 };
            vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, 40, pc);
            vkCmdDraw(cmd, 4, 1, 0, 0);
            if (!w.is_shell_ && decorator_) {
                // Decorator SSD chrome is positioned relative to the content rect,
                // not the full buffer. Use geometry dims when set.
                Window dw = w;
                if (has_geom) {
                    dw.size_ = { static_cast<float>(w.geom_w_),
                                 static_cast<float>(w.geom_h_) };
                }
                (void)decorator_->decorate(cmd, dw, it->second.set_,
                                            w.surface_id_ == focused_surface_id_);
            }
        }
    }

    // ── iOS-style GPU cursor blob (touchpad/tablet mode only) ──────────────────
    if (gpu_cursor_enabled_ && cursor_pipeline_ != VK_NULL_HANDLE) {
        // Step spring physics: compute dt from last frame
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - cursor_last_frame_).count();
        cursor_last_frame_ = now;
        dt = std::min(dt, 0.05f); // clamp to 50ms max

        // Update spring targets from raw cursor position
        cursor_state_.sx.setTarget(cx);
        cursor_state_.sy.setTarget(cy);
        cursor_state_.sx.step(dt);
        cursor_state_.sy.step(dt);
        cursor_state_.sw.step(dt);
        cursor_state_.sh.step(dt);
        cursor_state_.sr.step(dt);
        cursor_state_.salpha.step(dt);

        // Push constants: 10 floats = 40 bytes (fits pipeline_layout_flat_ 48-byte range)
        float cursor_pc[10] = {
            cursor_state_.sx.pos, cursor_state_.sy.pos,  // cursor_pos (vec2)
            cursor_state_.sw.pos, cursor_state_.sh.pos,  // blob_half (vec2)
            cursor_state_.sr.pos,                         // corner_radius
            6.0f,                                         // shadow_spread
            0.35f,                                        // shadow_alpha
            cursor_state_.salpha.pos,                    // fill_alpha
            static_cast<float>(ot.width),                // screen_size.x
            static_cast<float>(ot.height),               // screen_size.y
        };

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cursor_pipeline_);
        vkCmdPushConstants(cmd, pipeline_layout_flat_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, 40, cursor_pc);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }

    // Debug overlay: GPU timing HUD drawn on top of all windows (output 0 only).
    if (debug_overlay_visible_ && output_idx == 0)
        renderDebugOverlay(cmd, output_idx);

    vkCmdEndRenderPass(cmd);

    // Blit from optimal render target to linear scanout target
    VkImageMemoryBarrier copy_barriers[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,ot.render_image[buf_idx],{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,0,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,ot.target_image[buf_idx],{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}}
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 2, copy_barriers);

    VkImageCopy region{
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
        {ot.width, ot.height, 1}
    };
    vkCmdCopyImage(cmd, ot.render_image[buf_idx], VK_IMAGE_LAYOUT_GENERAL,
                        ot.target_image[buf_idx], VK_IMAGE_LAYOUT_GENERAL, 1, &region);

    // GPU timestamp: frame end (bottom-of-pipe), close debug label.
    if (profiler) profiler->endFrame(cmd);
    if (pfn_end_label_) pfn_end_label_(cmd);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO,0,0,0,0,1,&cmd,0,0};
    vkQueueSubmit(graphics_queue_, 1, &si, 0); vkQueueWaitIdle(graphics_queue_);

    // Read back timestamps after the GPU finishes.
    if (profiler) {
        profiler->resolveFrame();
        // Feed the primary output's frame time to com.freebsdx.FramePerf.
        if (output_idx == 0)
            FramePerfService::publish(profiler->lastMs(), profiler->frameCount());
    }

    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    ++ot.current_frame;
    return {};
}

// Backward-compat wrapper
std::expected<void, VulkanError> VulkanCompositor::composite(
    const WindowStack& ws, float cx, float cy, float dx, float dy, float dw, float dh) noexcept
{
    return composite(0, ws, cx, cy, dx, dy, dw, dh);
}

std::expected<int, VulkanError> VulkanCompositor::exportTarget(size_t output_idx) noexcept {
    if (output_idx >= output_targets_.size()) return std::unexpected(VulkanError::ImportFailed);
    auto& ot = output_targets_[output_idx];
    uint32_t buf_idx = (ot.current_frame - 1) % 2;
    VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,0,ot.target_memory[buf_idx],VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    int fd = -1; auto fp = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR");
    if (!fp || fp(device_, &gi, &fd) != VK_SUCCESS) return std::unexpected(VulkanError::ImportFailed);
    return fd;
}

// Backward-compat wrapper
std::expected<int, VulkanError> VulkanCompositor::exportTarget() noexcept {
    return exportTarget(0);
}

std::expected<std::pair<int, shared::ScreencopyFrameInfo>, VulkanError>
VulkanCompositor::captureToShm(size_t output_idx) noexcept {
    if (output_idx >= output_targets_.size())
        return std::unexpected(VulkanError::ImportFailed);

    auto& ot = output_targets_[output_idx];
    uint32_t buf_idx = (ot.current_frame > 0) ? (ot.current_frame - 1) % 2 : 0;
    uint32_t w = ot.width, h = ot.height;
    uint32_t stride = w * 4; // XRGB8888 = 4 bytes/pixel
    VkDeviceSize total = static_cast<VkDeviceSize>(stride) * h;

    // 1. Host-visible staging buffer for image readback
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0,
                           total, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
    VkBuffer sbuf = VK_NULL_HANDLE;
    if (vkCreateBuffer(device_, &bci, nullptr, &sbuf) != VK_SUCCESS)
        return std::unexpected(VulkanError::ImportFailed);

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device_, sbuf, &mr);

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                             mr.size,
                             findMemoryType(mr.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory smem = VK_NULL_HANDLE;
    if (vkAllocateMemory(device_, &mai, nullptr, &smem) != VK_SUCCESS) {
        vkDestroyBuffer(device_, sbuf, nullptr);
        return std::unexpected(VulkanError::ImportFailed);
    }
    vkBindBufferMemory(device_, sbuf, smem, 0);

    // 2. One-shot command buffer: GENERAL → TRANSFER_SRC, copy, restore
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                     nullptr, command_pool_,
                                     VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &cbai, &cmd) != VK_SUCCESS) {
        vkDestroyBuffer(device_, sbuf, nullptr); vkFreeMemory(device_, smem, nullptr);
        return std::unexpected(VulkanError::ImportFailed);
    }

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &cbbi);

    auto barrier = [&](VkAccessFlags src, VkAccessFlags dst,
                       VkImageLayout ol, VkImageLayout nl,
                       VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
                               src, dst, ol, nl,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                               ot.target_image[buf_idx],
                               {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    barrier(VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy reg{};
    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    reg.imageExtent      = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, ot.target_image[buf_idx],
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sbuf, 1, &reg);

    barrier(VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr,
                    0, nullptr, nullptr, 1, &cmd, 0, nullptr};
    vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);

    // 3. Map staging, write to anonymous shared memory
    void* mapped = nullptr;
    vkMapMemory(device_, smem, 0, total, 0, &mapped);

    int shm_fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
    if (shm_fd < 0) {
        vkUnmapMemory(device_, smem);
        vkDestroyBuffer(device_, sbuf, nullptr); vkFreeMemory(device_, smem, nullptr);
        return std::unexpected(VulkanError::ImportFailed);
    }
    ftruncate(shm_fd, static_cast<off_t>(total));

    void* shm_ptr = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr != MAP_FAILED) {
        std::memcpy(shm_ptr, mapped, total);
        munmap(shm_ptr, total);
    }

    vkUnmapMemory(device_, smem);
    vkDestroyBuffer(device_, sbuf, nullptr);
    vkFreeMemory(device_, smem, nullptr);

    shared::ScreencopyFrameInfo info{ w, h, stride, 2 /* WL_SHM_FORMAT_XRGB8888 */ };
    std::cout << "[vulkan] captureToShm: " << w << "x" << h << " output[" << output_idx << "]\n";
    return std::make_pair(shm_fd, info);
}

uint32_t VulkanCompositor::getStride(size_t output_idx) const noexcept {
    if (output_idx >= output_targets_.size()) return 0;
    return output_targets_[output_idx].stride;
}

uint32_t VulkanCompositor::getCurrentFrame(size_t output_idx) const noexcept {
    if (output_idx >= output_targets_.size()) return 0;
    return output_targets_[output_idx].current_frame;
}

std::expected<std::vector<uint32_t>, VulkanError> VulkanCompositor::readbackPixels(uint32_t w, uint32_t h) noexcept {
    if (output_targets_.empty()) return std::unexpected(VulkanError::ImportFailed);
    auto& ot = output_targets_[0];
    // Read from the buffer that held the most-recently composited frame.
    // composite() writes to target_image[(current_frame-1) % 2] and then
    // increments current_frame, so the last written slot is:
    uint32_t last_idx = (ot.current_frame == 0) ? 0 : (ot.current_frame - 1) % 2;
    VkDeviceSize s = static_cast<VkDeviceSize>(w) * h * 4;
    VkBuffer sb; VkDeviceMemory sm;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,0,0,s,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_SHARING_MODE_EXCLUSIVE,0,0};
    vkCreateBuffer(device_, &bci, 0, &sb);
    VkMemoryRequirements rmr; vkGetBufferMemoryRequirements(device_, sb, &rmr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,0,rmr.size,findMemoryType(rmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    vkAllocateMemory(device_, &ai, 0, &sm); vkBindBufferMemory(device_, sb, sm, 0);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,0,command_pool_,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
    VkCommandBuffer cmd; vkAllocateCommandBuffers(device_, &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,0,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,0};
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier tb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,ot.target_image[last_idx],{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &tb);
    VkBufferImageCopy r{0,0,0,{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},{w,h,1}};
    vkCmdCopyImageToBuffer(cmd, ot.target_image[last_idx], VK_IMAGE_LAYOUT_GENERAL, sb, 1, &r);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO,0,0,0,0,1,&cmd,0,0};
    vkQueueSubmit(graphics_queue_, 1, &si, 0); vkQueueWaitIdle(graphics_queue_);
    void* d; vkMapMemory(device_, sm, 0, s, 0, &d);
    std::vector<uint32_t> p(w * h); std::memcpy(p.data(), d, static_cast<size_t>(s));
    vkUnmapMemory(device_, sm); vkDestroyBuffer(device_, sb, 0); vkFreeMemory(device_, sm, 0);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd); return p;
}

std::expected<std::vector<uint8_t>, VulkanError> VulkanCompositor::inspectSurfacePixels(VkImage surface_image, uint32_t w, uint32_t h) noexcept {
    (void)surface_image; (void)w; (void)h;
    return std::vector<uint8_t>();
}

uint32_t VulkanCompositor::getFrameChecksum() noexcept {
    auto p = readbackPixels(20, 20); if (!p) return 0;
    uint32_t sum = 0; for (uint32_t v : *p) sum += v; return sum;
}

std::expected<VkShaderModule, VulkanError> VulkanCompositor::createShaderModule(const std::string& f) const noexcept {
    // Resolve shaders relative to the executable, not the CWD. The paths passed
    // in are "freebsdx.p/<name>.spv", which only exist next to the binary (in
    // out/). If freebsdx is launched from anywhere other than out/ (e.g. the repo
    // root via ./out/freebsdx), a bare CWD-relative open fails — the pipeline is
    // then null and NOTHING gets composited (blank screen). Try the CWD path
    // first (dev convenience), then fall back to <exe_dir>/<path>.
    auto try_open = [](const std::string& path) -> std::ifstream {
        return std::ifstream(path, std::ios::ate | std::ios::binary);
    };
    std::ifstream file = try_open(f);
    if (!file.is_open()) {
        char buf[4096];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::string exe(buf);
            auto slash = exe.find_last_of('/');
            if (slash != std::string::npos)
                file = try_open(exe.substr(0, slash + 1) + f);
        }
    }
    if (!file.is_open()) {
        std::cerr << "[compositor] shader not found (CWD or exe-relative): " << f << "\n";
        return std::unexpected(VulkanError::ImportFailed);
    }
    size_t s = (size_t)file.tellg(); std::vector<char> b(s); file.seekg(0); file.read(b.data(), s);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,0,0,b.size(),reinterpret_cast<const uint32_t*>(b.data())};
    VkShaderModule m; if (vkCreateShaderModule(device_, &ci, 0, &m) != VK_SUCCESS) return std::unexpected(VulkanError::ImportFailed);
    return m;
}

uint32_t VulkanCompositor::findMemoryType(uint32_t f, VkMemoryPropertyFlags p) const noexcept {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physical_device_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) { if ((f & (1 << i)) && (mp.memoryTypes[i].propertyFlags & p) == p) return i; }
    return 0;
}

void VulkanCompositor::trimOutputs(size_t count) noexcept {
    if (!device_ || count >= output_targets_.size()) return;
    vkDeviceWaitIdle(device_);
    for (size_t i = count; i < output_targets_.size(); ++i)
        destroyOutputTarget(output_targets_[i]);
    output_targets_.resize(count);
    std::cout << "[xserver] Compositor trimmed to " << count << " output target(s).\n";
}

void VulkanCompositor::releaseImage(VkImage image) noexcept {
    if (descriptor_sets_.contains(image)) {
        auto& res = descriptor_sets_[image];
        vkDestroyImageView(device_, res.view_, nullptr);
        vkFreeMemory(device_, res.memory_, nullptr);
        vkDestroyImage(device_, image, nullptr);
        // Return descriptor set to the reuse pool rather than abandoning it.
        freeDescriptorSet(res.set_);
        descriptor_sets_.erase(image);
    }
}

// ─── Pipeline cache ────────────────────────────────────────────────────────────

static std::string pipelineCachePath() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.freebsdx-vk-cache"
                : "/tmp/freebsdx-vk-cache";
}

void VulkanCompositor::loadPipelineCache() noexcept {
    std::vector<char> data;

    std::ifstream f(pipelineCachePath(), std::ios::binary | std::ios::ate);
    if (f.is_open()) {
        std::streamsize sz = f.tellg();
        if (sz >= 32) { // minimum valid Vulkan pipeline cache header
            f.seekg(0);
            data.resize(static_cast<size_t>(sz));
            f.read(data.data(), sz);

            // Validate header against physical device to reject stale caches.
            // Header layout (spec §10.6): length(4) version(4) vendorID(4)
            // deviceID(4) pipelineCacheUUID(16) = 32 bytes total.
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(physical_device_, &props);
            const uint32_t* hdr = reinterpret_cast<const uint32_t*>(data.data());
            if (hdr[0] < 32 ||                    // header length
                hdr[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
                hdr[2] != props.vendorID ||
                hdr[3] != props.deviceID ||
                std::memcmp(data.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
                std::cout << "[VulkanCompositor] Pipeline cache header mismatch — ignoring stale cache\n";
                data.clear();
            }
        }
        f.close();
    }

    VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0,
        data.size(), data.empty() ? nullptr : data.data()};
    if (vkCreatePipelineCache(device_, &pcci, nullptr, &pipeline_cache_) == VK_SUCCESS) {
        std::cout << "[VulkanCompositor] Pipeline cache loaded ("
                  << data.size() << " bytes from disk)\n";
    } else {
        // Create empty cache; pipeline creation still works, just uncached.
        VkPipelineCacheCreateInfo empty{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, 0, nullptr};
        vkCreatePipelineCache(device_, &empty, nullptr, &pipeline_cache_);
        std::cerr << "[VulkanCompositor] Failed to load pipeline cache — starting fresh\n";
    }
}

void VulkanCompositor::savePipelineCache() noexcept {
    if (pipeline_cache_ == VK_NULL_HANDLE) return;
    size_t sz = 0;
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &sz, nullptr) != VK_SUCCESS || sz == 0)
        return;
    std::vector<char> data(sz);
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &sz, data.data()) != VK_SUCCESS)
        return;
    std::ofstream f(pipelineCachePath(), std::ios::binary | std::ios::trunc);
    if (f.is_open()) {
        f.write(data.data(), static_cast<std::streamsize>(sz));
        std::cout << "[VulkanCompositor] Pipeline cache saved (" << sz << " bytes)\n";
    }
}

// ─── Descriptor-set reuse pool ─────────────────────────────────────────────────

VkDescriptorSet VulkanCompositor::allocateDescriptorSet() noexcept {
    if (!ds_free_list_.empty()) {
        VkDescriptorSet s = ds_free_list_.back();
        ds_free_list_.pop_back();
        return s;
    }
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        nullptr, descriptor_pool_, 1, &descriptor_set_layout_};
    VkDescriptorSet s = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(device_, &dsai, &s);
    return s;
}

void VulkanCompositor::freeDescriptorSet(VkDescriptorSet set) noexcept {
    if (set != VK_NULL_HANDLE)
        ds_free_list_.push_back(set);
}

void VulkanCompositor::renderDebugOverlay(VkCommandBuffer cmd, size_t output_idx) noexcept {
    if (output_idx >= output_targets_.size()) return;
    auto& ot = output_targets_[output_idx];
    float sw = static_cast<float>(ot.width);
    float sh = static_cast<float>(ot.height);

    // ── Background panel (270 × 62 at top-left) ──────────────────────────────
    constexpr float px = 10.0f, py = 10.0f, pw = 270.0f, ph = 62.0f;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, solid_pipeline_);
    float bgpc[10] = { px, py, pw, ph, sw, sh, 0.04f, 0.04f, 0.06f, 0.84f };
    vkCmdPushConstants(cmd, pipeline_layout_flat_,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 40, bgpc);
    vkCmdDraw(cmd, 4, 1, 0, 0);

    // ── Text lines ────────────────────────────────────────────────────────────
    if (!title_pipeline_ || !title_text_shaper_ || !title_font_atlas_) return;

    const GpuProfiler* prof = (output_idx < gpu_profilers_.size())
                            ? gpu_profilers_[output_idx].get() : nullptr;
    float last_ms = prof ? prof->lastMs()    : 0.0f;
    float avg_ms  = prof ? prof->averageMs() : 0.0f;
    uint64_t frames = prof ? prof->frameCount() : 0;

    char line1[64], line2[64];
    std::snprintf(line1, sizeof(line1), "GPU frame: %.2f ms", last_ms);
    std::snprintf(line2, sizeof(line2), "Avg (%d): %.2f ms   #%llu",
        GpuProfiler::kHistorySize, avg_ms, static_cast<unsigned long long>(frames));

    constexpr float kFontSize = 12.0f;
    constexpr float kAtlasRef = 48.0f;
    float scale    = kFontSize / kAtlasRef;
    constexpr float atlas_w = static_cast<float>(xrender::GlyphCache::kPageSize);
    constexpr float atlas_h = static_cast<float>(xrender::GlyphCache::kPageSize);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, title_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        title_pipeline_layout_, 0, 1, &title_desc_set_, 0, nullptr);

    auto renderLine = [&](const char* text, float pen_x, float pen_y) {
        auto* face = xrender::FontManager::getFace();
        auto glyphs = (face && title_text_shaper_)
            ? title_text_shaper_->shapeText(std::string(text), face->hb_font_, face->face_id_)
            : std::vector<xrender::ShapedGlyph>{};
        for (const auto& glyph : glyphs) {
            const auto* m = title_font_atlas_->lookup(glyph.face_id_, glyph.glyph_index_);
            if (!m || m->width_ == 0.0f) { pen_x += glyph.x_advance_ * scale; continue; }
            float gx = pen_x + (m->bearing_x_ + glyph.x_offset_) * scale;
            float gy = pen_y - (m->bearing_y_ + glyph.y_offset_) * scale;
            float gw = m->width_  * scale;
            float gh = m->height_ * scale;
            float tpc[14] = {
                gx, gy, gw, gh, sw, sh,
                0.20f, 1.0f, 0.45f, 1.0f, // green-on-dark, readable at a glance
                m->atlas_u_, m->atlas_v_,
                m->width_ / atlas_w, m->height_ / atlas_h
            };
            vkCmdPushConstants(cmd, title_pipeline_layout_,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 56, tpc);
            vkCmdDraw(cmd, 4, 1, 0, 0);
            pen_x += glyph.x_advance_ * scale;
        }
    };

    renderLine(line1, px + 8.0f, py + 20.0f);
    renderLine(line2, px + 8.0f, py + 46.0f);
}

void VulkanCompositor::initTitleTextPipeline() noexcept {
    // 1. Initialise font system and create the glyph cache
    xrender::FontManager::init();
    title_font_atlas_  = xrender::GlyphCache::create();
    if (!title_font_atlas_) {
        std::cerr << "[compositor] Font atlas creation failed — title text disabled\n";
        return;
    }
    title_text_shaper_ = std::make_unique<xrender::TextShaper>();

    // 2. Upload atlas bitmap to a Vulkan R8_UNORM image
    // The atlas starts empty; if no glyphs have been rasterised yet, skip upload.
    if (title_font_atlas_->getPageCount() == 0) {
        std::cerr << "[compositor] Font atlas is empty — title text disabled\n";
        return;
    }
    constexpr uint32_t aw = xrender::GlyphCache::kPageSize;
    constexpr uint32_t ah = xrender::GlyphCache::kPageSize;
    const auto& bitmap = title_font_atlas_->getPageBitmap(0);
    VkDeviceSize atlas_size = static_cast<VkDeviceSize>(aw) * ah;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr, 0,
        VK_IMAGE_TYPE_2D, VK_FORMAT_R8_UNORM, {aw, ah, 1},
        1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED};
    vkCreateImage(device_, &ii, nullptr, &title_atlas_image_);

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device_, title_atlas_image_, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
        mr.size, findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    vkAllocateMemory(device_, &mai, nullptr, &title_atlas_memory_);
    vkBindImageMemory(device_, title_atlas_image_, title_atlas_memory_, 0);

    // Upload via staging buffer
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, atlas_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
    VkBuffer sb; VkDeviceMemory sm;
    vkCreateBuffer(device_, &bci, nullptr, &sb);
    VkMemoryRequirements br; vkGetBufferMemoryRequirements(device_, sb, &br);
    VkMemoryAllocateInfo bmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, br.size,
        findMemoryType(br.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    vkAllocateMemory(device_, &bmai, nullptr, &sm);
    vkBindBufferMemory(device_, sb, sm, 0);
    void* mapped; vkMapMemory(device_, sm, 0, atlas_size, 0, &mapped);
    std::memcpy(mapped, bitmap.data(), static_cast<size_t>(atlas_size));
    vkUnmapMemory(device_, sm);

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
        command_pool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(device_, &cbai, &cmd);
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    vkBeginCommandBuffer(cmd, &cbbi);

    VkImageMemoryBarrier b1{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, title_atlas_image_,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b1);
    VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0,0,0}, {aw, ah, 1}};
    vkCmdCopyBufferToImage(cmd, sb, title_atlas_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier b2 = b1;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b2);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd, 0, nullptr};
    vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    vkDestroyBuffer(device_, sb, nullptr);
    vkFreeMemory(device_, sm, nullptr);

    // 3. Create image view + sampler
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, title_atlas_image_,
        VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8_UNORM, {}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCreateImageView(device_, &vi, nullptr, &title_atlas_view_);

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, nullptr, 0,
        VK_FILTER_LINEAR, VK_FILTER_LINEAR,
        VK_SAMPLER_MIPMAP_MODE_NEAREST,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        0, VK_FALSE, 1.0f, VK_FALSE, VK_COMPARE_OP_ALWAYS, 0, 0,
        VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_FALSE};
    vkCreateSampler(device_, &sci, nullptr, &title_sampler_);

    // 4. Descriptor set layout + pool + set
    VkDescriptorSetLayoutBinding dslb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
        VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        nullptr, 0, 1, &dslb};
    vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &title_desc_set_layout_);

    VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        nullptr, 0, 1, 1, &dps};
    vkCreateDescriptorPool(device_, &dpci, nullptr, &title_desc_pool_);

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
        title_desc_pool_, 1, &title_desc_set_layout_};
    vkAllocateDescriptorSets(device_, &dsai, &title_desc_set_);

    VkDescriptorImageInfo dii{title_sampler_, title_atlas_view_,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        title_desc_set_, 0, 0, 1,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii, nullptr, nullptr};
    vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);

    // 5. Pipeline layout (push constants 56 bytes)
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 56};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0,
        1, &title_desc_set_layout_, 1, &pcr};
    vkCreatePipelineLayout(device_, &plci, nullptr, &title_pipeline_layout_);

    // 6. Create text pipeline using ui_text shaders
    auto vm = createShaderModule("freebsdx.p/ui_text.vert.spv");
    auto fm = createShaderModule("freebsdx.p/ui_text.frag.spv");
    if (!vm || !fm) {
        std::cerr << "[compositor] Failed to load ui_text shaders — title text disabled\n";
        return;
    }
    VkPipelineShaderStageCreateInfo ss[] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_VERTEX_BIT,   vm.value(), "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_FRAGMENT_BIT, fm.value(), "main", nullptr}
    };
    VkPipelineVertexInputStateCreateInfo vis{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0, 0, nullptr, 0, nullptr};
    VkPipelineInputAssemblyStateCreateInfo ias{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_FALSE};
    VkViewport vp{0,0,1920,1080,0,1}; VkRect2D sc{{0,0},{1920,1080}};
    VkPipelineViewportStateCreateInfo vps{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_FALSE,
        VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE,
        VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, nullptr, 0,
        VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 1.0f, nullptr, VK_FALSE, VK_FALSE};
    VkPipelineColorBlendAttachmentState ba{VK_TRUE,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
        VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xf};
    VkPipelineColorBlendStateCreateInfo bs{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0,
        VK_FALSE, VK_LOGIC_OP_COPY, 1, &ba, {0,0,0,0}};
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, dynStates};
    VkGraphicsPipelineCreateInfo pi{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, nullptr, 0,
        2, ss, &vis, &ias, nullptr, &vps, &rs, &ms, nullptr, &bs, &dsi,
        title_pipeline_layout_, render_pass_, 0, VK_NULL_HANDLE, 0};
    if (vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &pi, nullptr, &title_pipeline_) != VK_SUCCESS) {
        std::cerr << "[compositor] Failed to create title text pipeline\n";
        vkDestroyShaderModule(device_, vm.value(), nullptr);
        vkDestroyShaderModule(device_, fm.value(), nullptr);
        return;
    }
    vkDestroyShaderModule(device_, vm.value(), nullptr);
    vkDestroyShaderModule(device_, fm.value(), nullptr);

    // 7. Pass everything to the decorator
    decorator_->setTitleTextResources(
        title_pipeline_, title_pipeline_layout_,
        title_desc_set_,
        title_font_atlas_.get(), title_text_shaper_.get());

    std::cout << "[compositor] Title text pipeline ready\n";
}

void VulkanCompositor::setGpuCursorEnabled(bool enabled) noexcept {
    gpu_cursor_enabled_ = enabled;
    if (enabled) {
        // Snap spring to current raw position to avoid jump on enable
        cursor_state_.sx.snap();
        cursor_state_.sy.snap();
        cursor_last_frame_ = std::chrono::steady_clock::now();
    }
}

void VulkanCompositor::updateCursorPosition(float x, float y) noexcept {
    if (!gpu_cursor_enabled_) return;
    cursor_state_.raw_x = x;
    cursor_state_.raw_y = y;
    // Target positions update — spring steps happen during composite()
    // If we have a hover target, morph to it when pointer is close enough
    if (cursor_state_.has_hover) {
        float dx = x - cursor_state_.hover_cx;
        float dy = y - cursor_state_.hover_cy;
        float dist2 = dx*dx + dy*dy;
        float max_hw = std::max(cursor_state_.hover_hw, cursor_state_.hover_hh) + 30.f;
        if (dist2 < max_hw * max_hw) {
            // Morph to button shape
            cursor_state_.sx.setTarget(cursor_state_.hover_cx);
            cursor_state_.sy.setTarget(cursor_state_.hover_cy);
            cursor_state_.sw.setTarget(cursor_state_.hover_hw);
            cursor_state_.sh.setTarget(cursor_state_.hover_hh);
            float min_dim = std::min(cursor_state_.hover_hw, cursor_state_.hover_hh);
            cursor_state_.sr.setTarget(std::min(min_dim, 8.f));
            cursor_state_.salpha.setTarget(0.55f);
            return;
        }
    }
    // Default: small circular blob following pointer
    cursor_state_.sx.setTarget(x);
    cursor_state_.sy.setTarget(y);
    cursor_state_.sw.setTarget(11.f);
    cursor_state_.sh.setTarget(11.f);
    cursor_state_.sr.setTarget(11.f);
    cursor_state_.salpha.setTarget(0.72f);
}

void VulkanCompositor::setCursorHoverTarget(float cx, float cy, float hw, float hh) noexcept {
    cursor_state_.has_hover = true;
    cursor_state_.hover_cx = cx;
    cursor_state_.hover_cy = cy;
    cursor_state_.hover_hw = hw;
    cursor_state_.hover_hh = hh;
}

void VulkanCompositor::clearCursorHoverTarget() noexcept {
    cursor_state_.has_hover = false;
}

} // namespace fbx::xserver
