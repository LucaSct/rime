// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "rime/core/containers/slot_map.hpp"
#include "rime/platform/native_window.hpp"
#include "rime/rhi/command_buffer.hpp"
#include "rime/rhi/device.hpp"
#include "rime/rhi/swapchain.hpp"
#include "vulkan/vulkan_common.hpp"

// The concrete Vulkan implementation of the RHI. VulkanDevice owns the instance/device/queues/VMA
// allocator and a SlotMap per resource kind; VulkanCommandBuffer records into a VkCommandBuffer.
// Both are defined across device_vulkan.cpp, resources_vulkan.cpp, pipeline_vulkan.cpp and
// command_buffer_vulkan.cpp — split by concern but sharing these declarations.
namespace rime::rhi {

// Backend resource records. The public API only ever sees opaque handles; these are what those
// handles resolve to inside the SlotMaps.
struct VulkanBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo info{}; // info.pMappedData is non-null for host-visible (mapped) allocations
    VkDeviceSize size = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryUsage memory = MemoryUsage::GpuOnly;
};

struct VulkanTexture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    Extent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    TextureUsage usage = TextureUsage::None;
    // The image's current layout, tracked so the command encoder can insert the right transition.
    // Single-threaded recording in M3 makes this simple bookkeeping; the render graph will own
    // layout/lifetime tracking properly later.
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    // True for a swapchain backbuffer: the VkImage is owned by the VkSwapchainKHR, not VMA, so
    // teardown destroys our view but must NOT vmaDestroyImage it (the swapchain frees the image).
    bool from_swapchain = false;
};

struct VulkanShader {
    VkShaderModule module = VK_NULL_HANDLE;
    ShaderStage stage = ShaderStage::Vertex;
    std::string entry_point = "main";
};

struct VulkanPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    // Set when the pipeline samples a texture (GraphicsPipelineDesc::sampled_texture): the descriptor
    // set-0 layout, plus a lazily-allocated, cached descriptor set that bind_texture fills in. M3.5
    // has a single static material, so the set is allocated once and reused every frame; bound_*
    // record what it currently points at, so we only re-write it when the texture/sampler change.
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    TextureHandle bound_texture{};
    SamplerHandle bound_sampler{};
    // Stage mask of the pipeline's push-constant range (0 = none). push_constants() needs it because
    // vkCmdPushConstants must be told which stages the data is for, and it must match the range.
    VkShaderStageFlags push_constant_stages = 0;
};

struct VulkanSampler {
    VkSampler sampler = VK_NULL_HANDLE;
};

class VulkanCommandBuffer; // defined below; VulkanDevice::submit_with_sync takes one by reference

class VulkanDevice final : public Device {
public:
    // Brings up Vulkan end to end. Returns nullptr (after logging) if no loader/ICD is present or no
    // GPU meets the Vulkan 1.3 + dynamic-rendering + synchronization2 bar — so callers can degrade.
    static std::unique_ptr<VulkanDevice> create(const DeviceDesc& desc);
    ~VulkanDevice() override;

    [[nodiscard]] const AdapterInfo& adapter() const override { return adapter_; }

    [[nodiscard]] BufferHandle create_buffer(const BufferDesc& desc) override;
    [[nodiscard]] TextureHandle create_texture(const TextureDesc& desc) override;
    [[nodiscard]] ShaderHandle create_shader(const ShaderDesc& desc) override;
    [[nodiscard]] PipelineHandle create_graphics_pipeline(const GraphicsPipelineDesc& desc) override;
    [[nodiscard]] SamplerHandle create_sampler(const SamplerDesc& desc) override;

    void destroy(BufferHandle handle) override;
    void destroy(TextureHandle handle) override;
    void destroy(ShaderHandle handle) override;
    void destroy(PipelineHandle handle) override;
    void destroy(SamplerHandle handle) override;

    void write_buffer(BufferHandle handle, const void* data, std::size_t size, std::size_t offset)
        override;
    void read_buffer(BufferHandle handle, void* dst, std::size_t size, std::size_t offset) override;
    void write_texture(TextureHandle handle, const void* data, std::size_t size) override;

    [[nodiscard]] std::unique_ptr<CommandBuffer> begin_commands() override;
    void submit_blocking(CommandBuffer& commands) override;
    void wait_idle() override;

    [[nodiscard]] std::unique_ptr<Swapchain> create_swapchain(const SwapchainDesc& desc) override;

    // ── Internals used by VulkanCommandBuffer (same module) ──────────────────────────────────
    [[nodiscard]] VkDevice vk_device() const noexcept { return device_; }
    [[nodiscard]] VulkanBuffer* lookup(BufferHandle h) noexcept {
        return buffers_.get(rebrand<VulkanBuffer>(h));
    }
    [[nodiscard]] VulkanTexture* lookup(TextureHandle h) noexcept {
        return textures_.get(rebrand<VulkanTexture>(h));
    }
    [[nodiscard]] VulkanPipeline* lookup(PipelineHandle h) noexcept {
        return pipelines_.get(rebrand<VulkanPipeline>(h));
    }
    [[nodiscard]] VulkanSampler* lookup(SamplerHandle h) noexcept {
        return samplers_.get(rebrand<VulkanSampler>(h));
    }
    // The shared pool bind_texture allocates its (cached, per-pipeline) descriptor set from.
    [[nodiscard]] VkDescriptorPool descriptor_pool() const noexcept { return descriptor_pool_; }

    // ── Internals used by VulkanSwapchain (same module) ──────────────────────────────────────
    [[nodiscard]] VkInstance vk_instance() const noexcept { return instance_; }
    [[nodiscard]] VkPhysicalDevice vk_physical() const noexcept { return physical_; }
    [[nodiscard]] std::uint32_t graphics_family() const noexcept { return graphics_family_; }
    [[nodiscard]] VkQueue graphics_queue() const noexcept { return graphics_queue_; }
    [[nodiscard]] VkCommandPool vk_command_pool() const noexcept { return command_pool_; }

    // End + submit `cmd` with this frame's synchronization (wait the image-acquired semaphore at the
    // color-output stage, signal render-finished, trip the in-flight fence) — the present-paced
    // counterpart to submit_blocking(). Does not wait or free the command buffer: the swapchain
    // waits the fence and frees the buffer when the frame slot recurs.
    void submit_with_sync(VulkanCommandBuffer& cmd, VkSemaphore wait, VkSemaphore signal, VkFence fence);

    // Register/forget a swapchain backbuffer as a texture so it flows through the normal command
    // path (begin_rendering, transitions). adopt returns the public handle; forget erases the slot
    // only — the swapchain owns and frees the VkImage/VkImageView itself.
    [[nodiscard]] TextureHandle adopt_swapchain_image(const VulkanTexture& t) {
        return rebrand<Texture>(textures_.insert(t));
    }
    void forget_texture(TextureHandle h) noexcept { textures_.erase(rebrand<VulkanTexture>(h)); }

private:
    VulkanDevice() = default;

    // create() steps, each returns false on failure (after logging the cause).
    bool create_instance(const DeviceDesc& desc);
    bool create_debug_messenger();
    bool pick_physical_device();
    bool create_logical_device();
    bool create_allocator();
    bool create_command_pool();
    bool create_descriptor_pool();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t graphics_family_ = 0;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE; // M3.5: combined image-sampler sets
    VmaAllocator allocator_ = nullptr;
    bool validation_ = false;

    AdapterInfo adapter_{};

    core::SlotMap<VulkanBuffer> buffers_;
    core::SlotMap<VulkanTexture> textures_;
    core::SlotMap<VulkanShader> shaders_;
    core::SlotMap<VulkanPipeline> pipelines_;
    core::SlotMap<VulkanSampler> samplers_;
};

class VulkanCommandBuffer final : public CommandBuffer {
public:
    VulkanCommandBuffer(VulkanDevice& device, VkCommandBuffer cmd) noexcept
        : device_(device), cmd_(cmd) {}

    void begin_rendering(const RenderingInfo& info) override;
    void end_rendering() override;
    void bind_pipeline(PipelineHandle pipeline) override;
    void bind_vertex_buffer(BufferHandle buffer, std::uint64_t offset) override;
    void bind_index_buffer(BufferHandle buffer, IndexType type, std::uint64_t offset) override;
    void bind_texture(std::uint32_t binding, TextureHandle texture, SamplerHandle sampler) override;
    void push_constants(const void* data, std::uint32_t size, std::uint32_t offset) override;
    void set_viewport(const Viewport& viewport) override;
    void set_scissor(const Rect2D& scissor) override;
    void draw(std::uint32_t vertex_count,
              std::uint32_t instance_count,
              std::uint32_t first_vertex,
              std::uint32_t first_instance) override;
    void draw_indexed(std::uint32_t index_count,
                      std::uint32_t instance_count,
                      std::uint32_t first_index,
                      std::int32_t vertex_offset,
                      std::uint32_t first_instance) override;
    void copy_texture_to_buffer(TextureHandle src, BufferHandle dst) override;

    [[nodiscard]] VkCommandBuffer handle() const noexcept { return cmd_; }

private:
    VulkanDevice& device_;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VulkanPipeline* current_pipeline_ = nullptr; // set by bind_pipeline; bind_texture needs its layout
};

// Create a VkSurfaceKHR for a platform window. The one OS-touching spot in the backend: it switches
// on NativeWindow.system and calls the matching vkCreate*SurfaceKHR, each guarded by the platform's
// VK_USE_PLATFORM_* macro (set per-OS in CMakeLists.txt, mirroring engine/platform's per-OS backend
// selection). Returns VK_NULL_HANDLE on failure (after logging). Defined in surface_vulkan.cpp.
[[nodiscard]] VkSurfaceKHR create_surface(VkInstance instance,
                                          const platform::NativeWindow& window) noexcept;

// The Vulkan swapchain: a VkSurfaceKHR + VkSwapchainKHR, its backbuffer images (registered as
// textures so begin_rendering can target them), and the per-frame synchronization that overlaps CPU
// recording with GPU execution (frames-in-flight). Created from a VulkanDevice; see swapchain.hpp.
class VulkanSwapchain final : public Swapchain {
public:
    static std::unique_ptr<VulkanSwapchain> create(VulkanDevice& device, const SwapchainDesc& desc);
    ~VulkanSwapchain() override;

    [[nodiscard]] TextureHandle acquire_next_image() override;
    bool present(CommandBuffer& commands) override;
    void recreate(Extent2D extent) override;
    [[nodiscard]] Format format() const override { return rhi_format_; }
    [[nodiscard]] Extent2D extent() const override { return {extent_.width, extent_.height}; }

private:
    explicit VulkanSwapchain(VulkanDevice& device) noexcept : device_(device) {}
    bool build(Extent2D extent);          // (re)create the swapchain, its images/views, per-image sync
    void destroy_swapchain_objects() noexcept; // images/views/swapchain (keeps the surface + per-frame sync)

    // Two frames in flight: while the GPU works on frame N the CPU records frame N+1. Each in-flight
    // slot owns an image-available semaphore, an in-flight fence, and the command buffer it last
    // submitted (freed when the slot recurs). render-finished is per *image* (not per frame) so a
    // present never waits on a semaphore still pending from a different image — the standard fix.
    static constexpr std::uint32_t kFramesInFlight = 2;

    VulkanDevice& device_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat vk_format_ = VK_FORMAT_UNDEFINED;
    Format rhi_format_ = Format::Undefined;
    VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent_{};
    bool vsync_ = true;

    std::vector<VkImage> images_;             // owned by the swapchain
    std::vector<VkImageView> views_;          // owned by us, one per image
    std::vector<TextureHandle> handles_;      // the RHI handle each image is registered under
    std::vector<VkSemaphore> render_finished_; // one per image

    std::array<VkSemaphore, kFramesInFlight> image_available_{};
    std::array<VkFence, kFramesInFlight> in_flight_{};
    std::array<VkCommandBuffer, kFramesInFlight> frame_cmd_{}; // deferred free (in flight until slot recurs)
    std::uint32_t frame_ = 0;        // current in-flight slot
    std::uint32_t image_index_ = 0;  // last acquired swapchain image index
};

// Factory used by the agnostic create_device() in src/device.cpp. Declared here (not in a public
// header) so the agnostic glue stays free of Vulkan types — it only forward-declares this symbol.
[[nodiscard]] std::unique_ptr<Device> create_vulkan_device(const DeviceDesc& desc);

} // namespace rime::rhi
