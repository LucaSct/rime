# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Conan recipe declaring Rime's C++ third-party dependencies.

Why Conan: see docs/adr/0001 (C++ core) and the dependency-management choice recorded
for Milestone 0. Conan resolves/builds our native dependencies and hands CMake a
toolchain file that knows where to find them.

The two-step workflow (wrapped by scripts/build so it's one command in practice):

    1. conan install . -of build/<preset> -s build_type=<Cfg> --build=missing
         -> writes conan_toolchain.cmake (+ <dep>-config.cmake) into build/<preset>/
    2. cmake --preset <preset>          # its toolchainFile points at the file above
       cmake --build --preset <preset>

We deliberately keep the dependency set small and intentional (see
third_party/README.md): every dependency is code we ship and are bound by.
"""

from conan import ConanFile


class RimeRecipe(ConanFile):
    name = "rime"
    # Engine version — keep in sync with engine/core/include/rime/core/version.hpp.
    version = "0.0.1"

    # Settings that affect the binaries Conan selects/builds for us.
    settings = "os", "compiler", "build_type", "arch"

    # Generators emit CMake integration files into the install output folder:
    #   CMakeToolchain -> conan_toolchain.cmake (compiler, flags, prefix paths)
    #   CMakeDeps      -> <dep>-config.cmake files so find_package() locates each dep
    generators = "CMakeToolchain", "CMakeDeps"

    def requirements(self):
        # fmt: fast, type-safe formatting. It is our FIRST dependency for two reasons:
        #   1. It proves the Conan -> CMake pipeline end-to-end in Milestone 0.
        #   2. It will become the basis of the engine's logging in Milestone 1.
        self.requires("fmt/10.2.1")

        # ── RHI Vulkan backend (Milestone 3) ──────────────────────────────────────────
        # We deliberately depend on only what we *build* against, not what we *run* against.
        # volk is a Vulkan "meta-loader": it dlopen()s the platform Vulkan loader
        # (libvulkan.{so,dylib} / vulkan-1.dll) at runtime via volkInitialize(), so we link
        # NO loader at build time -- only the headers (types/enums) and volk's dispatch table.
        # The runtime loader *and* an ICD (a real GPU driver, MoltenVK on macOS, or lavapipe in
        # CI) are provided by the environment, not Conan -- the same dev-vs-CI split M2 used for
        # windows. We use the newest stable headers; the *runtime* baseline we require is Vulkan
        # 1.3 (dynamic rendering + synchronization2) -- see docs/adr/0007.
        self.requires("vulkan-headers/1.4.313.0")
        self.requires("volk/1.4.313.0")
        # VMA (Vulkan Memory Allocator): header-only, battle-tested GPU allocator. We never
        # call vkAllocateMemory directly; VMA sub-allocates from a few big device allocations.
        self.requires("vulkan-memory-allocator/3.3.0")

        # doctest: a fast-compiling, header-only unit-test framework. Declared as a
        # *test* requirement -- it is needed to build and run our tests, but it is not
        # part of the engine we ship, so it must never leak to consumers of Rime. Conan
        # keeps test_requires out of the package's runtime requirement graph.
        self.test_requires("doctest/2.4.11")

    def build_requirements(self):
        # glslang gives us glslangValidator, which compiles our GLSL shaders to SPIR-V at
        # build time (ADR-0008 -- offline compilation, no runtime shader compiler shipped).
        # A *build-context* tool: it runs on the build machine and is never part of Rime.
        self.tool_requires("glslang/1.4.313.0")
