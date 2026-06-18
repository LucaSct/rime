// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <memory>
#include <string>

#include "rime/core/containers/slot_map.hpp"
#include "rime/rhi/command_buffer.hpp"
#include "rime/rhi/device.hpp"
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
};

struct VulkanShader {
    VkShaderModule module = VK_NULL_HANDLE;
    ShaderStage stage = ShaderStage::Vertex;
    std::string entry_point = "main";
};

struct VulkanPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};

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

    void destroy(BufferHandle handle) override;
    void destroy(TextureHandle handle) override;
    void destroy(ShaderHandle handle) override;
    void destroy(PipelineHandle handle) override;

    void write_buffer(BufferHandle handle, const void* data, std::size_t size, std::size_t offset)
        override;
    void read_buffer(BufferHandle handle, void* dst, std::size_t size, std::size_t offset) override;

    [[nodiscard]] std::unique_ptr<CommandBuffer> begin_commands() override;
    void submit_blocking(CommandBuffer& commands) override;
    void wait_idle() override;

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

private:
    VulkanDevice() = default;

    // create() steps, each returns false on failure (after logging the cause).
    bool create_instance(const DeviceDesc& desc);
    bool create_debug_messenger();
    bool pick_physical_device();
    bool create_logical_device();
    bool create_allocator();
    bool create_command_pool();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t graphics_family_ = 0;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    bool validation_ = false;

    AdapterInfo adapter_{};

    core::SlotMap<VulkanBuffer> buffers_;
    core::SlotMap<VulkanTexture> textures_;
    core::SlotMap<VulkanShader> shaders_;
    core::SlotMap<VulkanPipeline> pipelines_;
};

class VulkanCommandBuffer final : public CommandBuffer {
public:
    VulkanCommandBuffer(VulkanDevice& device, VkCommandBuffer cmd) noexcept
        : device_(device), cmd_(cmd) {}

    void begin_rendering(const RenderingInfo& info) override;
    void end_rendering() override;
    void bind_pipeline(PipelineHandle pipeline) override;
    void bind_vertex_buffer(BufferHandle buffer, std::uint64_t offset) override;
    void set_viewport(const Viewport& viewport) override;
    void set_scissor(const Rect2D& scissor) override;
    void draw(std::uint32_t vertex_count,
              std::uint32_t instance_count,
              std::uint32_t first_vertex,
              std::uint32_t first_instance) override;
    void copy_texture_to_buffer(TextureHandle src, BufferHandle dst) override;

    [[nodiscard]] VkCommandBuffer handle() const noexcept { return cmd_; }

private:
    VulkanDevice& device_;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
};

// Factory used by the agnostic create_device() in src/device.cpp. Declared here (not in a public
// header) so the agnostic glue stays free of Vulkan types — it only forward-declares this symbol.
[[nodiscard]] std::unique_ptr<Device> create_vulkan_device(const DeviceDesc& desc);

} // namespace rime::rhi
