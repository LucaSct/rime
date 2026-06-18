// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The single translation unit that compiles the Vulkan Memory Allocator implementation. VMA ships
// as one big header; defining VMA_IMPLEMENTATION in exactly one .cpp emits its function bodies here,
// and every other TU includes the header for declarations only. This file is built in its own
// static lib (rime_vma) with default warning flags, because VMA is third-party code we don't hold
// to Rime's -Werror — the same arrangement as rime_xdg_shell for the Wayland protocol glue.
//
// We pair VMA with volk (the meta-loader): VK_NO_PROTOTYPES means there are no statically linked
// vk* symbols, so VMA is told to load what it needs dynamically (VMA_DYNAMIC_VULKAN_FUNCTIONS) from
// the vkGetInstanceProcAddr / vkGetDeviceProcAddr pointers we hand it at allocator creation.

#include <volk.h> // brings in the Vulkan types (with prototypes disabled by volk)

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
