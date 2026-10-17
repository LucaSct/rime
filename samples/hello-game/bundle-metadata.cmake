# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
# Runtime inventory reviewed for hello-game's Linux/Vulkan linker closure. Conan build tools and
# streaming codecs are not redistributed by this game; generated Wayland code IS incorporated.
set(HELLO_GAME_BUNDLE_METADATA "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND RIME_RHI_VULKAN AND NOT CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _bundle_config)
    set(_notice_fields "profile=hello-game-linux-vulkan-v1\n")
    foreach(_component fmt volk vulkan-memory-allocator vulkan-headers)
        set(_folder_var "${_component}_PACKAGE_FOLDER_${_bundle_config}")
        if(NOT IS_DIRECTORY "${${_folder_var}}/licenses")
            message(FATAL_ERROR "hello-game bundle audit: missing ${_component} notice directory")
        endif()
        string(APPEND _notice_fields "notice_${_component}=${${_folder_var}}/licenses\n")
    endforeach()
    string(APPEND _notice_fields
        "reference_fmt=fmt/${fmt_VERSION_STRING}\n"
        "reference_volk=volk/${volk_VERSION_STRING}\n"
        "reference_vulkan-memory-allocator=vulkan-memory-allocator/${VulkanMemoryAllocator_VERSION_STRING}\n"
        "reference_vulkan-headers=vulkan-headers/${VulkanHeaders_VERSION_STRING}\n"
        "notice_rime=${PROJECT_SOURCE_DIR}/LICENSE\n"
        "notice_rime_notice=${PROJECT_SOURCE_DIR}/NOTICE\n"
        "wayland_generated=${CMAKE_BINARY_DIR}/engine/platform/wayland\n")
    set(HELLO_GAME_BUNDLE_METADATA "${CMAKE_CURRENT_BINARY_DIR}/hello-game-bundle.notices")
    file(GENERATE OUTPUT "${HELLO_GAME_BUNDLE_METADATA}" CONTENT "${_notice_fields}")
endif()
