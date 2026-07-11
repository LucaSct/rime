# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.

# Out-of-tree consumer of the installed Rime SDK (M6.8). This is a SEPARATE CMake project — it is
# never add_subdirectory()'d by the engine (the top-level CMakeLists does not descend here), so it
# sees Rime ONLY through the installed package, exactly as a downstream game would.
# scripts/sdk-smoke.sh configures it standalone against a throwaway install prefix, builds it, and
# runs it. If a public header failed to install, or the export set dropped a transitive usage
# requirement, this project is where it surfaces — that is the whole point of the proof.
cmake_minimum_required(VERSION 3.24)
project(rime_sdk_consumer LANGUAGES CXX)

# The one line a downstream project writes. find_package locates rime-config.cmake via rime_DIR (or
# CMAKE_PREFIX_PATH) — the smoke script points it at <prefix>/lib/cmake/rime. The config re-finds
# Rime's third-party dependencies, which is why the consumer must be configured with the same Conan
# toolchain the engine was built with (the v1 dependency stance — see docs/design/sdk.md).
find_package(rime CONFIG REQUIRED)

add_executable(sdk_consumer main.cpp)
target_compile_features(sdk_consumer PRIVATE cxx_std_20)

# Link the highest-level modules this app uses; their PUBLIC interfaces pull the rest transitively
# (rime::app → render/rhi/ecs/platform/core; rime::assets → core). Naming only what we use is the
# honest consumer shape — and the strongest test of the export set's transitive correctness.
target_link_libraries(sdk_consumer PRIVATE rime::app rime::assets)
