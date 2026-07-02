// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Vulkan device bring-up: loader -> instance (+ validation) -> physical device -> logical device +
// queue -> VMA allocator -> command pool, plus command submission. This is the M3.1 brick — it
// stands up a *headless* device (no surface/swapchain), which is exactly what off-screen rendering
// needs and what lets the proof run on a software GPU (lavapipe) in CI. Presentation is added in
// M3.4 by creating a swapchain from a platform::NativeWindow.

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "vulkan/vulkan_backend.hpp"

namespace rime::rhi {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

// Validation/diagnostic messages from the driver land here. We route by severity to Rime's log;
// returning VK_FALSE means "do not abort the call that produced the message" (per the spec — the
// messenger is an observer, not a gate).
VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* /*user*/) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        RIME_ERROR("[vulkan] {}", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        RIME_WARN("[vulkan] {}", data->pMessage);
    } else {
        RIME_DEBUG("[vulkan] {}", data->pMessage);
    }
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT make_messenger_info() {
    VkDebugUtilsMessengerCreateInfoEXT ci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debug_callback;
    return ci;
}

bool has_layer(const std::vector<VkLayerProperties>& layers, const char* name) {
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, name) == 0)
            return true;
    }
    return false;
}

bool has_ext(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0)
            return true;
    }
    return false;
}

std::optional<std::uint32_t> find_graphics_family(VkPhysicalDevice pd) {
    std::uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, fams.data());
    for (std::uint32_t i = 0; i < n; ++i) {
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            return i;
    }
    return std::nullopt;
}

} // namespace

std::unique_ptr<VulkanDevice> VulkanDevice::create(const DeviceDesc& desc) {
    // volk dynamically loads the Vulkan loader. Failing here is the expected, graceful outcome on a
    // machine with no Vulkan at all (no driver, no software ICD) — we return null so callers skip.
    if (volkInitialize() != VK_SUCCESS) {
        RIME_WARN(
            "rhi: no Vulkan loader present (volkInitialize failed) — no GPU device available");
        return nullptr;
    }

    auto dev = std::unique_ptr<VulkanDevice>(new VulkanDevice());
    dev->validation_ = desc.enable_validation;

    if (!dev->create_instance(desc))
        return nullptr;
    if (dev->validation_)
        dev->create_debug_messenger(); // best-effort; absence is not fatal
    if (!dev->pick_physical_device())
        return nullptr;
    if (!dev->create_logical_device())
        return nullptr;
    if (!dev->create_allocator())
        return nullptr;
    if (!dev->create_command_pool())
        return nullptr;
    if (!dev->create_descriptor_pool())
        return nullptr;

    RIME_INFO("rhi: Vulkan device ready — GPU '{}' (Vulkan {}.{}.{}){}",
              dev->adapter_.name,
              VK_API_VERSION_MAJOR(dev->adapter_.api_version),
              VK_API_VERSION_MINOR(dev->adapter_.api_version),
              VK_API_VERSION_PATCH(dev->adapter_.api_version),
              dev->validation_ ? ", validation on" : "");
    return dev;
}

VulkanDevice::~VulkanDevice() {
    if (device_)
        vkDeviceWaitIdle(device_);

    // Tear down in reverse dependency order. Resources first (some via VMA), then the allocator,
    // then the device, then instance-level objects.
    for (auto& p : pipelines_) {
        if (p.pipeline)
            vkDestroyPipeline(device_, p.pipeline, nullptr);
        if (p.layout)
            vkDestroyPipelineLayout(device_, p.layout, nullptr);
        if (p.set_layout)
            vkDestroyDescriptorSetLayout(device_, p.set_layout, nullptr);
    }
    for (auto& smp : samplers_) {
        if (smp.sampler)
            vkDestroySampler(device_, smp.sampler, nullptr);
    }
    for (auto& s : shaders_) {
        if (s.module)
            vkDestroyShaderModule(device_, s.module, nullptr);
    }
    for (auto& t : textures_) {
        // Swapchain backbuffers own their image+view; the VulkanSwapchain frees them in its own
        // destructor (which runs first, before the device). Skip them here so we never double-free.
        if (t.from_swapchain)
            continue;
        if (t.view)
            vkDestroyImageView(device_, t.view, nullptr);
        if (t.image)
            vmaDestroyImage(allocator_, t.image, t.allocation);
    }
    for (auto& b : buffers_) {
        if (b.buffer)
            vmaDestroyBuffer(allocator_, b.buffer, b.allocation);
    }

    if (command_pool_)
        vkDestroyCommandPool(device_, command_pool_, nullptr);
    if (descriptor_pool_)
        vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (allocator_)
        vmaDestroyAllocator(allocator_);
    if (device_)
        vkDestroyDevice(device_, nullptr);
    if (messenger_)
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_)
        vkDestroyInstance(instance_, nullptr);
}

bool VulkanDevice::create_instance(const DeviceDesc& desc) {
    std::uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers.data());

    std::uint32_t ext_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, exts.data());

    std::vector<const char*> enabled_layers;
    std::vector<const char*> enabled_exts;

    // Only enable validation if both the layer and the debug-utils extension are actually present —
    // otherwise drop it gracefully (e.g. a CI image without the validation layers installed).
    if (validation_) {
        if (has_layer(layers, kValidationLayer) &&
            has_ext(exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            enabled_layers.push_back(kValidationLayer);
            enabled_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            RIME_WARN("rhi: validation requested but layer/debug-utils unavailable — disabling");
            validation_ = false;
        }
    }

    // MoltenVK and other Metal/translation drivers are "portability" implementations; the loader
    // hides them unless we opt in by enabling this extension and flag (a no-op on Win/Linux GPUs).
    VkInstanceCreateFlags flags = 0;
    if (has_ext(exts, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        enabled_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    // Surface extensions (M3.4): enable VK_KHR_surface + this OS's window-system surface
    // extension(s) when present, so a Swapchain can later be created from a platform window. The
    // Device stays window-agnostic — we only make presentation *possible*; a headless build
    // (lavapipe in CI) has none of these and the off-screen path needs no surface, so this degrades
    // gracefully. The VK_USE_PLATFORM_* macros are set per-OS in CMakeLists.txt (Linux compiles
    // both Xlib + Wayland).
    if (has_ext(exts, VK_KHR_SURFACE_EXTENSION_NAME)) {
        enabled_exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(VK_USE_PLATFORM_METAL_EXT)
        if (has_ext(exts, VK_EXT_METAL_SURFACE_EXTENSION_NAME))
            enabled_exts.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        if (has_ext(exts, VK_KHR_WIN32_SURFACE_EXTENSION_NAME))
            enabled_exts.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
        if (has_ext(exts, VK_KHR_XLIB_SURFACE_EXTENSION_NAME))
            enabled_exts.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
        if (has_ext(exts, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME))
            enabled_exts.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#endif
    }

    const std::string app_name(desc.app_name);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = app_name.c_str();
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 0, 1);
    app.pEngineName = "Rime";
    app.engineVersion = VK_MAKE_API_VERSION(0, 0, 0, 1);
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.flags = flags;
    ci.pApplicationInfo = &app;
    ci.enabledLayerCount = static_cast<std::uint32_t>(enabled_layers.size());
    ci.ppEnabledLayerNames = enabled_layers.data();
    ci.enabledExtensionCount = static_cast<std::uint32_t>(enabled_exts.size());
    ci.ppEnabledExtensionNames = enabled_exts.data();

    // Chaining the messenger info on pNext also catches messages emitted *during* instance
    // creation.
    VkDebugUtilsMessengerCreateInfoEXT dbg = make_messenger_info();
    if (validation_)
        ci.pNext = &dbg;

    const VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreateInstance failed: {}", result_string(r));
        return false;
    }
    volkLoadInstanceOnly(instance_); // load instance-level entry points through volk
    return true;
}

bool VulkanDevice::create_debug_messenger() {
    VkDebugUtilsMessengerCreateInfoEXT ci = make_messenger_info();
    const VkResult r = vkCreateDebugUtilsMessengerEXT(instance_, &ci, nullptr, &messenger_);
    if (r != VK_SUCCESS) {
        RIME_WARN("rhi: failed to create debug messenger: {}", result_string(r));
        return false;
    }
    return true;
}

bool VulkanDevice::pick_physical_device() {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        RIME_ERROR("rhi: no Vulkan physical devices found");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties best_props{};
    int best_score = -1;

    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_3)
            continue;

        // Require our 1.3 baseline features (ADR-0007): dynamic rendering + synchronization2.
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        if (!f13.dynamicRendering || !f13.synchronization2)
            continue;
        if (!find_graphics_family(pd).has_value())
            continue;

        // Prefer a real discrete GPU, but a CPU device (lavapipe) is perfectly acceptable — it is
        // exactly what we want in CI, where there is no hardware GPU.
        int score = 0;
        switch (props.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                score = 1000;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                score = 500;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                score = 250;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                score = 100;
                break;
            default:
                score = 10;
                break;
        }
        if (score > best_score) {
            best_score = score;
            best = pd;
            best_props = props;
        }
    }

    if (best == VK_NULL_HANDLE) {
        RIME_ERROR("rhi: no GPU meets the Vulkan 1.3 + dynamic-rendering + synchronization2 bar");
        return false;
    }

    physical_ = best;
    graphics_family_ = *find_graphics_family(best);
    adapter_.name = best_props.deviceName;
    adapter_.vendor_id = best_props.vendorID;
    adapter_.device_id = best_props.deviceID;
    adapter_.type = to_rhi(best_props.deviceType);
    adapter_.api_version = best_props.apiVersion;
    return true;
}

bool VulkanDevice::create_logical_device() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = graphics_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    std::uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &ext_count, exts.data());

    std::vector<const char*> enabled_exts;
    // The spec *requires* enabling portability_subset whenever a device exposes it (MoltenVK does).
    if (has_ext(exts, "VK_KHR_portability_subset")) {
        enabled_exts.push_back("VK_KHR_portability_subset");
    }
    // VK_KHR_swapchain (M3.4): the device-level half of presentation. Enabled when present so a
    // Swapchain can be created; absent on a headless software ICD, where we never present.
    if (has_ext(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        enabled_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f2; // features passed via the features2 chain, so pEnabledFeatures stays null
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = static_cast<std::uint32_t>(enabled_exts.size());
    ci.ppEnabledExtensionNames = enabled_exts.data();

    const VkResult r = vkCreateDevice(physical_, &ci, nullptr, &device_);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreateDevice failed: {}", result_string(r));
        return false;
    }
    volkLoadDevice(device_); // device-level entry points (faster than going through the loader)
    vkGetDeviceQueue(device_, graphics_family_, 0, &graphics_queue_);
    return true;
}

bool VulkanDevice::create_allocator() {
    // VMA needs to call Vulkan, but with volk there are no statically linked vk* symbols — so we
    // hand it the two proc-address getters volk loaded, and VMA fetches the rest itself
    // (VMA_DYNAMIC_VULKAN_FUNCTIONS, set in vma_impl.cpp).
    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo ci{};
    ci.physicalDevice = physical_;
    ci.device = device_;
    ci.instance = instance_;
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.pVulkanFunctions = &fns;

    const VkResult r = vmaCreateAllocator(&ci, &allocator_);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vmaCreateAllocator failed: {}", result_string(r));
        return false;
    }
    return true;
}

bool VulkanDevice::create_command_pool() {
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = graphics_family_;
    const VkResult r = vkCreateCommandPool(device_, &ci, nullptr, &command_pool_);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreateCommandPool failed: {}", result_string(r));
        return false;
    }
    return true;
}

bool VulkanDevice::create_descriptor_pool() {
    // A small shared pool for M3.5's combined image-sampler descriptor sets. Each sampling pipeline
    // allocates one set, cached and reused across frames (the textured quad is static), so a
    // handful is ample; the M5 render graph will own a real per-frame descriptor allocator instead.
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 16;

    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = 16;
    ci.poolSizeCount = 1;
    ci.pPoolSizes = &pool_size;
    const VkResult r = vkCreateDescriptorPool(device_, &ci, nullptr, &descriptor_pool_);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreateDescriptorPool failed: {}", result_string(r));
        return false;
    }
    return true;
}

std::unique_ptr<CommandBuffer> VulkanDevice::begin_commands() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = command_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device_, &ai, &cmd));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    return std::make_unique<VulkanCommandBuffer>(*this, cmd);
}

void VulkanDevice::submit_blocking(CommandBuffer& commands) {
    // Only one CommandBuffer implementation exists; the RHI hands these out itself, so the cast is
    // safe. submit + wait is the simplest correct model (perfect for the M3 one-shot render);
    // overlapping frames with the swapchain arrives in M3.4.
    auto& vcb = static_cast<VulkanCommandBuffer&>(commands);
    VkCommandBuffer cmd = vcb.handle();
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo csi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    csi.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &csi;

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device_, &fci, nullptr, &fence));
    VK_CHECK(vkQueueSubmit2(graphics_queue_, 1, &si, fence));
    VK_CHECK(vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(device_, fence, nullptr);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
}

void VulkanDevice::wait_idle() {
    if (device_)
        vkDeviceWaitIdle(device_);
}

std::unique_ptr<Swapchain> VulkanDevice::create_swapchain(const SwapchainDesc& desc) {
    return VulkanSwapchain::create(*this, desc);
}

void VulkanDevice::submit_with_sync(VulkanCommandBuffer& vcb,
                                    VkSemaphore wait,
                                    VkSemaphore signal,
                                    VkFence fence) {
    VkCommandBuffer cmd = vcb.handle();
    VK_CHECK(vkEndCommandBuffer(cmd));

    // synchronization2 submit: wait the image-acquired semaphore, signal render-finished, trip the
    // in-flight fence. We wait at COLOR_ATTACHMENT_OUTPUT so earlier pipeline stages can overlap
    // the acquire, and only the color write blocks on the image being ready.
    VkSemaphoreSubmitInfo wait_si{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait_si.semaphore = wait;
    wait_si.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signal_si{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal_si.semaphore = signal;
    signal_si.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkCommandBufferSubmitInfo csi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    csi.commandBuffer = cmd;

    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait_si;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &csi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal_si;
    VK_CHECK(vkQueueSubmit2(graphics_queue_, 1, &si, fence));
}

std::unique_ptr<Device> create_vulkan_device(const DeviceDesc& desc) {
    return VulkanDevice::create(desc);
}

} // namespace rime::rhi
