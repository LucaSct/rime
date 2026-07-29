# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Conan recipe for SVT-AV1 4.x — the streaming encoder (ADR-0017, ADR-0030, ADR-0034).

WHY THIS RECIPE EXISTS AT ALL
    Conan Center's newest `libsvtav1` recipe is 2.2.1; upstream shipped 4.2.0 in July 2026.
    We need 4.x for the rate-control and entropy-coding race fixes (see ADR-0034), so the
    recipe has to live here until Conan Center catches up. When it does, delete this
    directory and go back to a plain `self.requires("libsvtav1/<version>")`.

WHY IT IS A REWRITE RATHER THAN A COPY OF CONAN CENTER'S
    SVT-AV1 4.x deleted the CMake knobs the 2.x recipe drives, so a copied recipe would
    silently configure nothing:
      - `BUILD_ENC` / `BUILD_DEC`  — gone. The decoder was removed from the project; the
        encoder is now unconditional, so the build_encoder/build_decoder options went too.
      - `USE_EXTERNAL_CPUINFO` and the whole `cpuinfo` dependency — gone. 4.x does its own
        CPU feature detection, so requiring cpuinfo would pull in a library nothing links.
      - `ENABLE_NASM` — gone. x86 assembly is now gated on CMake's own
        `check_language(ASM_NASM)`, which means nasm must simply be *on PATH at build time*
        (>= 2.14, enforced by upstream) rather than switched on by a variable.
    What did NOT change is the part our CMake glue depends on: the library is still
    `SvtAv1Enc`, the headers still install to `include/svt-av1`, and the pkg-config name is
    still `SvtAv1Enc` — so the `libsvtav1::encoder` component target keeps working and
    /CMakeLists.txt needs no edit.
"""

import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get, rmdir

required_conan_version = ">=2.1"


class LibSvtAv1Conan(ConanFile):
    name = "libsvtav1"
    # Pinned in the recipe so a bare `conan export third_party/conan-recipes/libsvtav1` works
    # with no arguments (build.sh exports every recipe in that directory generically). Bumping
    # SVT-AV1 means editing this line, conandata.yml, and the require in /conanfile.py.
    version = "4.2.0"
    description = "SVT-AV1: a scalable, multi-core software AV1 encoder"
    license = "BSD-3-Clause"
    homepage = "https://gitlab.com/AOMediaCodec/SVT-AV1"
    topics = ("av1", "codec", "encoder", "video", "streaming")
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")
        # SVT-AV1 is a C project; its C++ settings would only pollute the package id.
        self.settings.rm_safe("compiler.cppstd")
        self.settings.rm_safe("compiler.libcxx")

    def layout(self):
        cmake_layout(self, src_folder="src")

    def build_requirements(self):
        self.tool_requires("cmake/[>=3.16]")
        if self.settings.arch in ("x86", "x86_64"):
            # Upstream rejects nasm < 2.14. Without nasm on PATH the build still succeeds but
            # falls back to C, which costs roughly an order of magnitude of encode speed — far
            # outside the real-time budget ADR-0030 §3 sets. So this is a hard build tool.
            self.tool_requires("nasm/2.16.01")

    def source(self):
        get(self, **self.conan_data["sources"][self.version], strip_root=True)

    def generate(self):
        tc = CMakeToolchain(self)
        # We link the library only; upstream's SvtAv1EncApp CLI and its test suite are pure
        # build time we would pay on every cold cache.
        tc.cache_variables["BUILD_APPS"] = False
        tc.cache_variables["BUILD_TESTING"] = False
        # LTO across a third-party static library is a known source of toolchain-specific
        # breakage (and it interferes with sanitizer builds, which is precisely the
        # configuration ADR-0034 needs to observe). Off, deliberately.
        tc.cache_variables["SVT_AV1_LTO"] = False
        # NOTE: BUILD_SHARED_LIBS is set for us by CMakeToolchain from `options.shared`.
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        # Both files are required by the third_party/ licensing policy: SVT-AV1 is
        # BSD-3-Clause *plus* the Alliance for Open Media Patent License 1.0, and the patent
        # grant is the half that makes AV1 safe to ship (ADR-0017).
        for license_file in ("LICENSE.md", "PATENTS.md"):
            copy(self, license_file, self.source_folder,
                 dst=os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()
        # Conan generates its own CMake config and .pc files; upstream's would shadow them.
        rmdir(self, os.path.join(self.package_folder, "lib", "pkgconfig"))
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))

    def package_info(self):
        # The `encoder` component keeps Conan Center's naming so `libsvtav1::encoder` — the
        # target /CMakeLists.txt already looks up — resolves unchanged across the bump.
        self.cpp_info.components["encoder"].libs = ["SvtAv1Enc"]
        self.cpp_info.components["encoder"].includedirs = ["include/svt-av1"]
        self.cpp_info.components["encoder"].set_property("pkg_config_name", "SvtAv1Enc")
        if self.settings.os in ("FreeBSD", "Linux"):
            self.cpp_info.components["encoder"].system_libs = ["pthread", "dl", "m"]
        elif self.settings.os == "Android":
            self.cpp_info.components["encoder"].system_libs = ["m"]
