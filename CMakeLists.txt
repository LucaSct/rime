# ──────────────────────────────────────────────────────────────────────────────
# Rime Engine — top-level build.
#
# This is the entry point for building the C++ engine. Each engine module
# (engine/<module>) is its own CMake target; this file sets project-wide policy
# and wires the modules together. The Rust tooling under tools/ builds separately
# with Cargo.
#
# Status: scaffolding. Module subdirectories are added as they land (Milestone 0+).
# Kept intentionally small and commented so it reads as documentation of the build.
# ──────────────────────────────────────────────────────────────────────────────
cmake_minimum_required(VERSION 3.24)

project(Rime
    VERSION 0.0.1
    DESCRIPTION "Rime — an open-source AAA-grade game engine"
    LANGUAGES CXX)

# ── Language policy ───────────────────────────────────────────────────────────
# C++20 today; we adopt C++23 features as they stabilize across our compilers.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)              # no compiler-specific dialects

# Export compile_commands.json so clangd / tooling work out of the box.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to a sane build type for single-config generators.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
endif()

# ── Options ───────────────────────────────────────────────────────────────────
# The RHI is backend-pluggable (see docs/adr/0002). Vulkan is the first backend;
# others are added later without touching the renderer.
option(RIME_RHI_VULKAN "Build the Vulkan RHI backend" ON)
option(RIME_BUILD_SAMPLES "Build example projects under samples/" ON)
option(RIME_BUILD_TESTS "Build unit tests" ON)
option(RIME_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# ── Where build outputs go ────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ── Common build policy ───────────────────────────────────────────────────────
# Apply Rime's shared compile options to a target: the C++ standard and a strong
# warning set. Centralized here so every module is built identically — this is the
# single place project-wide build policy lives.
function(rime_apply_common_target_options target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(RIME_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(RIME_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ── Offline shader compilation (GLSL -> SPIR-V at build time) ──────────────────────────
# Rime ships no runtime shader compiler (ADR-0008): shaders are compiled during the build and
# embedded as C arrays the consuming target #includes. rime_add_shaders is the front door; it
# mirrors the wayland-scanner custom-command codegen in engine/platform/CMakeLists.txt. For each
# GLSL source it (1) runs glslangValidator to make a .spv, then (2) runs a tiny CMake -P script to
# turn that .spv into "<name>.spv.h" defining `static const uint32_t <name>_spv[] = {...}`, and adds
# the generated directory to the target's include path. So `triangle.vert` becomes the header
# "triangle.vert.spv.h" with the symbol `triangle_vert_spv`.
set(RIME_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}/cmake")

function(rime_add_shaders target)
    if(NOT RIME_GLSL_COMPILER)
        # glslangValidator comes from the Conan `glslang` tool_requires (a build-context tool, so
        # Conan puts it on CMAKE_PROGRAM_PATH). NAMES covers the upstream rename glslangValidator->glslang.
        find_program(RIME_GLSL_COMPILER NAMES glslangValidator glslang)
    endif()
    if(NOT RIME_GLSL_COMPILER)
        message(FATAL_ERROR
            "rime_add_shaders: no GLSL->SPIR-V compiler found (expected glslangValidator from the "
            "Conan glslang tool_requires). Run scripts/build, or install the Vulkan SDK.")
    endif()

    set(_gen "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY "${_gen}")
    foreach(_src ${ARGN})
        # Accept both target-relative sources and absolute paths, so a test can compile the same
        # shaders a sample owns (the M3.3 proof and 01-hello-triangle share triangle.vert/.frag).
        if(IS_ABSOLUTE "${_src}")
            set(_srcpath "${_src}")
        else()
            set(_srcpath "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        endif()
        get_filename_component(_name "${_src}" NAME)    # e.g. triangle.vert
        string(MAKE_C_IDENTIFIER "${_name}" _ident)     # e.g. triangle_vert
        set(_spv "${_gen}/${_name}.spv")
        set(_hdr "${_gen}/${_name}.spv.h")
        add_custom_command(
            OUTPUT "${_spv}"
            COMMAND "${RIME_GLSL_COMPILER}" -V --target-env vulkan1.3 -o "${_spv}" "${_srcpath}"
            DEPENDS "${_srcpath}"
            COMMENT "glsl -> spirv: ${_name}"
            VERBATIM)
        add_custom_command(
            OUTPUT "${_hdr}"
            COMMAND ${CMAKE_COMMAND} -DSPV=${_spv} -DHDR=${_hdr} -DSYM=${_ident}_spv
                    -P "${RIME_CMAKE_DIR}/embed_spirv.cmake"
            DEPENDS "${_spv}" "${RIME_CMAKE_DIR}/embed_spirv.cmake"
            COMMENT "embed spirv: ${_name}.spv.h"
            VERBATIM)
        target_sources(${target} PRIVATE "${_hdr}")
    endforeach()
    target_include_directories(${target} PRIVATE "${_gen}")
endfunction()

# ── Third-party dependencies ────────────────────────────────────────────────────
# Resolved by Conan (see /conanfile.py) and located here via the CMakeDeps config files
# it generates. We find them once, at the top level, so the imported targets (e.g.
# fmt::fmt) are visible to every module below — find_package's imported targets are
# scoped to the defining directory and its children, and the root is parent to them all.
# This also mirrors Conan handing us a single toolchain for the whole project.
find_package(fmt REQUIRED)

# RHI Vulkan backend dependencies (Milestone 3). Found once here so the imported targets are
# visible to engine/rhi below. volk is the meta-loader (it dlopen()s the Vulkan loader at runtime,
# so we link no loader at build time); VMA is the GPU memory allocator. See docs/adr/0007 and
# conanfile.py. Only needed when the Vulkan backend is enabled.
if(RIME_RHI_VULKAN)
    find_package(VulkanHeaders REQUIRED)         # Vulkan::Headers
    find_package(volk REQUIRED)                  # volk::volk / volk::volk_headers
    find_package(VulkanMemoryAllocator REQUIRED) # GPUOpen::VulkanMemoryAllocator
endif()

# ── Modules ───────────────────────────────────────────────────────────────────
# Added bottom-up as each is implemented. The dependency order mirrors the layer cake
# in docs/ARCHITECTURE.md (a module may only depend on layers below it).
add_subdirectory(engine/core)     # foundation: version (M0); memory, math, jobs… (M1+)
add_subdirectory(engine/platform) # OS abstraction: window, input, files, threads (M2+)
add_subdirectory(engine/rhi)      # Render Hardware Interface + Vulkan backend (M3)
add_subdirectory(engine/app)      # launcher stub (M0) → application framework (later)

# Not yet wired — they land in later milestones/bricks (see docs/ROADMAP.md):
#   add_subdirectory(engine/ecs)         # the data-oriented world
#   add_subdirectory(engine/render)      # render graph + high-level renderer
#   add_subdirectory(engine/physics)     # rigid bodies, collision
#   add_subdirectory(engine/destruction) # part-based destruction (headline system)

# ── Samples ─────────────────────────────────────────────────────────────────────
# Runnable example projects that exercise the engine; each milestone's proof lives here.
# First up: M1.6's job-system core-saturation demo.
if(RIME_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

# ── Tests ───────────────────────────────────────────────────────────────────────
# Unit tests live under tests/ (one target per module; see docs/ROADMAP.md, "Testing &
# profiling"). doctest is resolved by Conan as a test-only dependency, so we only look
# for it when tests are enabled. enable_testing() turns on CTest, which `ctest --preset
# dev` then drives.
if(RIME_BUILD_TESTS)
    enable_testing()
    find_package(doctest REQUIRED)
    add_subdirectory(tests)
endif()

message(STATUS "Rime ${PROJECT_VERSION} — Targets: rime_core, rime_platform, rime_rhi, rime_hello.")
