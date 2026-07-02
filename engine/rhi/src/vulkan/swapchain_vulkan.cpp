// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The Vulkan swapchain (M3.4): surface -> swapchain -> backbuffer images -> present, with
// frames-in-flight. This is what puts the M3.3 triangle in a real window (and finally maps the M2
// Wayland surface, which only shows once a buffer is attached). Backbuffers are registered as
// ordinary RHI textures (device.adopt_swapchain_image) so the existing command encoder renders into
// them unchanged. Synchronization is the standard model: per-frame image-available semaphore +
// in-flight fence overlap CPU recording with GPU work; a per-image render-finished semaphore gates
// the present. See docs/adr/0009 and docs/design/rhi.md.

#include <algorithm>
#include <vector>

#include "vulkan/vulkan_backend.hpp"

namespace rime::rhi {
namespace {

// Pick a surface format: prefer 8-bit BGRA (the near-universal swapchain format), UNORM over SRGB
// so the presented color matches what the shader wrote (the off-screen proof is UNORM too). Fall
// back to whatever the surface lists first.
VkSurfaceFormatKHR choose_format(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;
    }
    return formats.front();
}

// FIFO is always supported and tear-free (vsync). Reach for MAILBOX (low-latency, may drop frames)
// only when the caller opted out of vsync and the surface offers it.
VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& modes, bool vsync) {
    if (!vsync) {
        for (auto m : modes) {
            if (m == VK_PRESENT_MODE_MAILBOX_KHR)
                return m;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

} // namespace

std::unique_ptr<VulkanSwapchain> VulkanSwapchain::create(VulkanDevice& device,
                                                         const SwapchainDesc& desc) {
    auto sc = std::unique_ptr<VulkanSwapchain>(new VulkanSwapchain(device));
    sc->vsync_ = desc.vsync;

    sc->surface_ = create_surface(device.vk_instance(), desc.window);
    if (sc->surface_ == VK_NULL_HANDLE)
        return nullptr; // create_surface logged the cause

    // The graphics queue must also support present to this surface. True for all our targets
    // (desktop GPUs, MoltenVK); we verify rather than assume, and fail loudly if not (M3.4 uses one
    // queue for both — a dedicated present queue is a later refinement if a target ever needs it).
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(
        device.vk_physical(), device.graphics_family(), sc->surface_, &supported);
    if (!supported) {
        RIME_ERROR("rhi: the graphics queue family cannot present to this surface");
        vkDestroySurfaceKHR(device.vk_instance(), sc->surface_, nullptr);
        sc->surface_ = VK_NULL_HANDLE;
        return nullptr;
    }

    // Per-frame sync (fixed count, kept across recreate): an image-available semaphore and an
    // in-flight fence per frame slot. Fences start signaled so the first acquire doesn't deadlock.
    VkDevice dev = device.vk_device();
    for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateSemaphore(dev, &si, nullptr, &sc->image_available_[i]) != VK_SUCCESS ||
            vkCreateFence(dev, &fi, nullptr, &sc->in_flight_[i]) != VK_SUCCESS) {
            RIME_ERROR("rhi: failed to create swapchain frame synchronization");
            return nullptr; // ~VulkanSwapchain cleans up whatever partial state exists
        }
    }

    if (!sc->build(desc.extent))
        return nullptr;
    RIME_INFO("rhi: swapchain ready — {}x{}, {} images, {}",
              sc->extent_.width,
              sc->extent_.height,
              static_cast<unsigned>(sc->images_.size()),
              sc->present_mode_ == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : "mailbox");
    return sc;
}

VulkanSwapchain::~VulkanSwapchain() {
    device_.wait_idle(); // no frame may still be using these objects when we free them
    VkDevice dev = device_.vk_device();
    destroy_swapchain_objects(); // images/views/swapchain + per-image semaphores + forget handles
    for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (image_available_[i])
            vkDestroySemaphore(dev, image_available_[i], nullptr);
        if (in_flight_[i])
            vkDestroyFence(dev, in_flight_[i], nullptr);
        if (frame_cmd_[i] != VK_NULL_HANDLE)
            vkFreeCommandBuffers(dev, device_.vk_command_pool(), 1, &frame_cmd_[i]);
    }
    if (surface_)
        vkDestroySurfaceKHR(device_.vk_instance(), surface_, nullptr);
}

void VulkanSwapchain::destroy_swapchain_objects() noexcept {
    VkDevice dev = device_.vk_device();
    for (auto h : handles_)
        device_.forget_texture(h); // erase slots; the GPU objects are ours below
    for (auto v : views_) {
        if (v)
            vkDestroyImageView(dev, v, nullptr);
    }
    for (auto s : render_finished_) {
        if (s)
            vkDestroySemaphore(dev, s, nullptr);
    }
    handles_.clear();
    views_.clear();
    images_.clear();
    render_finished_.clear();
    if (swapchain_) {
        vkDestroySwapchainKHR(dev, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool VulkanSwapchain::build(Extent2D extent) {
    VkPhysicalDevice phys = device_.vk_physical();
    VkDevice dev = device_.vk_device();

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface_, &caps);

    std::uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface_, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface_, &fmt_count, formats.data());
    if (formats.empty()) {
        RIME_ERROR("rhi: surface reports no supported formats");
        return false;
    }

    std::uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface_, &mode_count, modes.data());

    const VkSurfaceFormatKHR sf = choose_format(formats);
    vk_format_ = sf.format;
    color_space_ = sf.colorSpace;
    rhi_format_ = from_vk(sf.format);
    present_mode_ = choose_present_mode(modes, vsync_);

    // Size to the surface: when currentExtent is the 0xFFFFFFFF sentinel the surface defers to us
    // (Wayland), so clamp the requested framebuffer size to the allowed range; otherwise the
    // surface dictates the size (most platforms), so we use it verbatim.
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        extent_ = caps.currentExtent;
    } else {
        extent_.width =
            std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height =
            std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent_.width == 0 || extent_.height == 0) {
        return false; // minimized / zero-size: nothing to build this round
    }

    // Triple-buffer when allowed (minImageCount+1), clamped to maxImageCount (0 means unlimited).
    std::uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = image_count;
    ci.imageFormat = vk_format_;
    ci.imageColorSpace = color_space_;
    ci.imageExtent = extent_;
    ci.imageArrayLayers = 1;
    // Render into the images, and allow copying out of them (transfer src) so a backbuffer can also
    // feed a readback — mirroring the off-screen target's usage and keeping the proof paths
    // uniform.
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; // one queue family (graphics == present)
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = present_mode_;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain_; // hand the retiring swapchain's resources to the new one

    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR(dev, &ci, nullptr, &new_swapchain);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreateSwapchainKHR failed: {}", result_string(r));
        return false;
    }

    // The new swapchain exists; retire the previous generation (oldSwapchain let the driver reuse
    // it).
    destroy_swapchain_objects();
    swapchain_ = new_swapchain;

    std::uint32_t n = 0;
    vkGetSwapchainImagesKHR(dev, swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(dev, swapchain_, &n, images_.data());

    views_.resize(n, VK_NULL_HANDLE);
    handles_.resize(n);
    render_finished_.resize(n, VK_NULL_HANDLE);
    for (std::uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = images_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = vk_format_;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(dev, &vci, nullptr, &views_[i]) != VK_SUCCESS) {
            RIME_ERROR("rhi: failed to create a swapchain image view");
            return false;
        }

        // Register the backbuffer as a texture so begin_rendering()/transitions treat it like any
        // other color target; from_swapchain marks it so teardown never VMA-frees the image.
        VulkanTexture t;
        t.image = images_[i];
        t.view = views_[i];
        t.extent = {extent_.width, extent_.height};
        t.format = vk_format_;
        t.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
        t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        t.from_swapchain = true;
        handles_[i] = device_.adopt_swapchain_image(t);

        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(dev, &si, nullptr, &render_finished_[i]) != VK_SUCCESS) {
            RIME_ERROR("rhi: failed to create a swapchain present semaphore");
            return false;
        }
    }
    frame_ = 0;
    return true;
}

TextureHandle VulkanSwapchain::acquire_next_image() {
    VkDevice dev = device_.vk_device();
    VK_CHECK(vkWaitForFences(dev, 1, &in_flight_[frame_], VK_TRUE, UINT64_MAX));

    // The command buffer this slot submitted last cycle is now guaranteed complete — free it. (One
    // alloc/free per frame; command-buffer pooling is a labeled later optimization, see rhi.md.)
    if (frame_cmd_[frame_] != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(dev, device_.vk_command_pool(), 1, &frame_cmd_[frame_]);
        frame_cmd_[frame_] = VK_NULL_HANDLE;
    }

    std::uint32_t idx = 0;
    const VkResult r = vkAcquireNextImageKHR(
        dev, swapchain_, UINT64_MAX, image_available_[frame_], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
        return {}; // invalid handle: caller calls recreate()
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        RIME_ERROR("rhi: vkAcquireNextImageKHR failed: {}", result_string(r));
        return {};
    }
    vkResetFences(dev, 1, &in_flight_[frame_]); // unsignal only now that we know we'll submit
    image_index_ = idx;
    return handles_[idx];
}

bool VulkanSwapchain::present(CommandBuffer& commands) {
    auto& vcb = static_cast<VulkanCommandBuffer&>(commands);

    // Transition the just-rendered backbuffer from color-attachment to present layout, recorded
    // into the same command buffer right before we end + submit it.
    VulkanTexture* tex = device_.lookup(handles_[image_index_]);
    transition_image(vcb.handle(),
                     tex->image,
                     tex->layout,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                     0);
    tex->layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    device_.submit_with_sync(
        vcb, image_available_[frame_], render_finished_[image_index_], in_flight_[frame_]);
    frame_cmd_[frame_] = vcb.handle(); // alive until this slot recurs (freed in acquire_next_image)

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_finished_[image_index_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image_index_;
    const VkResult r = vkQueuePresentKHR(device_.graphics_queue(), &pi);

    frame_ = (frame_ + 1) % kFramesInFlight;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        return false; // caller recreates
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vkQueuePresentKHR failed: {}", result_string(r));
        return false;
    }
    return true;
}

void VulkanSwapchain::recreate(Extent2D extent) {
    device_.wait_idle();
    VkDevice dev = device_.vk_device();
    // Reset per-frame sync to a clean state: an image-available semaphore left signaled by an
    // acquire whose frame we abandoned (a resize between acquire and present) would trip validation
    // when reused, so recreate those semaphores and free any deferred command buffers before
    // rebuilding.
    for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (image_available_[i])
            vkDestroySemaphore(dev, image_available_[i], nullptr);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(dev, &si, nullptr, &image_available_[i]));
        if (frame_cmd_[i] != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(dev, device_.vk_command_pool(), 1, &frame_cmd_[i]);
            frame_cmd_[i] = VK_NULL_HANDLE;
        }
    }
    build(extent);
}

} // namespace rime::rhi
