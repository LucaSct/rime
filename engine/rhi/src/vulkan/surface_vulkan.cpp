// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// VkSurfaceKHR creation — the *one* place the Vulkan backend touches an OS windowing type. It takes
// the type-erased platform::NativeWindow (engine/platform's seam: {system, display, handle}, all
// void*) and calls the matching vkCreate*SurfaceKHR. Each branch is guarded by the platform's
// VK_USE_PLATFORM_* macro, set per-OS in CMakeLists.txt — exactly mirroring how engine/platform
// compiles one window backend per OS (Linux compiles BOTH Xlib and Wayland, since it picks at
// runtime). We resolve the platform surface entry point with vkGetInstanceProcAddr rather than
// volk's global function pointer, so this does not depend on how the prebuilt volk was configured.

#include <cstdint>

#include "vulkan/vulkan_backend.hpp"

namespace rime::rhi {

VkSurfaceKHR create_surface(VkInstance instance, const platform::NativeWindow& window) noexcept {
    using platform::WindowSystem;
    switch (window.system) {
#if defined(VK_USE_PLATFORM_METAL_EXT)
        case WindowSystem::Cocoa: {
            auto create = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT"));
            if (!create) {
                RIME_ERROR(
                    "rhi: vkCreateMetalSurfaceEXT unavailable (VK_EXT_metal_surface missing?)");
                return VK_NULL_HANDLE;
            }
            VkMetalSurfaceCreateInfoEXT ci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
            ci.pLayer =
                reinterpret_cast<const CAMetalLayer*>(window.handle); // CAMetalLayer* (M2 Cocoa)
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            const VkResult r = create(instance, &ci, nullptr, &surface);
            if (r != VK_SUCCESS) {
                RIME_ERROR("rhi: vkCreateMetalSurfaceEXT failed: {}", result_string(r));
                return VK_NULL_HANDLE;
            }
            return surface;
        }
#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        case WindowSystem::Win32: {
            auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
                vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
            if (!create) {
                RIME_ERROR("rhi: vkCreateWin32SurfaceKHR unavailable");
                return VK_NULL_HANDLE;
            }
            VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
            ci.hinstance = reinterpret_cast<HINSTANCE>(window.display);
            ci.hwnd = reinterpret_cast<HWND>(window.handle);
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            const VkResult r = create(instance, &ci, nullptr, &surface);
            if (r != VK_SUCCESS) {
                RIME_ERROR("rhi: vkCreateWin32SurfaceKHR failed: {}", result_string(r));
                return VK_NULL_HANDLE;
            }
            return surface;
        }
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
        case WindowSystem::Xlib: {
            auto create = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(
                vkGetInstanceProcAddr(instance, "vkCreateXlibSurfaceKHR"));
            if (!create) {
                RIME_ERROR("rhi: vkCreateXlibSurfaceKHR unavailable");
                return VK_NULL_HANDLE;
            }
            VkXlibSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
            ci.dpy = reinterpret_cast<Display*>(window.display);
            // NativeWindow stores the X11 window XID packed into a void*; unpack via uintptr_t.
            ci.window = static_cast<Window>(reinterpret_cast<std::uintptr_t>(window.handle));
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            const VkResult r = create(instance, &ci, nullptr, &surface);
            if (r != VK_SUCCESS) {
                RIME_ERROR("rhi: vkCreateXlibSurfaceKHR failed: {}", result_string(r));
                return VK_NULL_HANDLE;
            }
            return surface;
        }
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
        case WindowSystem::Wayland: {
            auto create = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
                vkGetInstanceProcAddr(instance, "vkCreateWaylandSurfaceKHR"));
            if (!create) {
                RIME_ERROR("rhi: vkCreateWaylandSurfaceKHR unavailable");
                return VK_NULL_HANDLE;
            }
            VkWaylandSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
            ci.display = reinterpret_cast<wl_display*>(window.display);
            ci.surface = reinterpret_cast<wl_surface*>(window.handle);
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            const VkResult r = create(instance, &ci, nullptr, &surface);
            if (r != VK_SUCCESS) {
                RIME_ERROR("rhi: vkCreateWaylandSurfaceKHR failed: {}", result_string(r));
                return VK_NULL_HANDLE;
            }
            return surface;
        }
#endif
        default:
            RIME_ERROR("rhi: no Vulkan surface support compiled in for this window system");
            return VK_NULL_HANDLE;
    }
}

} // namespace rime::rhi
