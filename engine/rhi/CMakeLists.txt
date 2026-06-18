# rime_rhi — the Render Hardware Interface and its Vulkan backend.
#
# This module is the graphics *seam* (ADR-0002). Public headers under include/rime/rhi/ are
# API-agnostic — they never include a Vulkan header. The Vulkan backend under src/vulkan/ is the
# ONLY code that does, and its dependencies (volk, the Vulkan headers, VMA) are linked PRIVATE, so
# nothing that links rime::rhi can even transitively see <vulkan.h>. The "no Vulkan upward" rule is
# thus enforced by the build, not just by review — the same idea as platform's "no OS #ifdef upward".

# Backend-agnostic sources — built on every platform and in every configuration.
add_library(rime_rhi STATIC
    src/device.cpp)

# Namespaced alias so dependents link rime::rhi (matching rime::core / rime::platform).
add_library(rime::rhi ALIAS rime_rhi)

# Public headers: the agnostic interface. Private src/ holds the backend's internal headers.
target_include_directories(rime_rhi
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Depends only on the layers below (the layer cake): core for handles/log/asserts, platform for the
# NativeWindow it will turn into a surface when presentation lands in M3.4.
target_link_libraries(rime_rhi
    PUBLIC rime::core rime::platform)

# ── Vulkan backend ────────────────────────────────────────────────────────────────────────────
if(RIME_RHI_VULKAN)
    target_sources(rime_rhi PRIVATE
        src/vulkan/device_vulkan.cpp
        src/vulkan/resources_vulkan.cpp
        src/vulkan/pipeline_vulkan.cpp
        src/vulkan/command_buffer_vulkan.cpp)

    # Lets the agnostic factory (src/device.cpp) compile in the call to the Vulkan backend.
    target_compile_definitions(rime_rhi PRIVATE RIME_RHI_VULKAN=1)

    # VMA is a single header whose *implementation* must be compiled in exactly one TU. It is
    # third-party C++ we don't hold to -Werror, so — exactly like rime_xdg_shell for the Wayland
    # protocol glue — it builds in its own static lib with default flags and is linked PRIVATE.
    add_library(rime_vma STATIC src/vulkan/vma_impl.cpp)
    target_link_libraries(rime_vma PRIVATE volk::volk_headers GPUOpen::VulkanMemoryAllocator)

    # All Vulkan-facing deps are PRIVATE: the seam. volk::volk carries the compiled volk.c (the
    # dynamically-loaded entry points); Vulkan::Headers the types; VMA the allocator declarations.
    target_link_libraries(rime_rhi PRIVATE
        volk::volk
        Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator
        rime_vma)
endif()

# Project-wide build policy (C++20 + the strong warning set), same as every module.
rime_apply_common_target_options(rime_rhi)

# One scoped exception to the warning set: the Vulkan struct-init idiom `VkFoo x{VK_STRUCTURE_TYPE_…}`
# deliberately zero-initializes every other field, which is exactly what -Wmissing-field-initializers
# (pulled in by -Wextra) flags — with no value for this code. Disable just that one warning for this
# module on GCC/Clang; everything else in -Wextra/-Werror still applies, and MSVC doesn't warn here.
if(NOT MSVC)
    target_compile_options(rime_rhi PRIVATE -Wno-missing-field-initializers)
endif()
