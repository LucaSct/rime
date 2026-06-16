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

        # doctest: a fast-compiling, header-only unit-test framework. Declared as a
        # *test* requirement -- it is needed to build and run our tests, but it is not
        # part of the engine we ship, so it must never leak to consumers of Rime. Conan
        # keeps test_requires out of the package's runtime requirement graph.
        self.test_requires("doctest/2.4.11")
