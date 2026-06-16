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

# ── Third-party dependencies ────────────────────────────────────────────────────
# Resolved by Conan (see /conanfile.py) and located here via the CMakeDeps config files
# it generates. We find them once, at the top level, so the imported targets (e.g.
# fmt::fmt) are visible to every module below — find_package's imported targets are
# scoped to the defining directory and its children, and the root is parent to them all.
# This also mirrors Conan handing us a single toolchain for the whole project.
find_package(fmt REQUIRED)

# ── Modules ───────────────────────────────────────────────────────────────────
# Added bottom-up as each is implemented. The dependency order mirrors the layer cake
# in docs/ARCHITECTURE.md (a module may only depend on layers below it).
add_subdirectory(engine/core)   # foundation: version (M0); memory, math, jobs… (M1+)
add_subdirectory(engine/app)    # launcher stub (M0) → application framework (later)

# Not yet wired — they land in later milestones/bricks (see docs/ROADMAP.md):
#   add_subdirectory(engine/platform)    # window, input, files, threads
#   add_subdirectory(engine/rhi)         # Render Hardware Interface (+ Vulkan backend)
#   add_subdirectory(engine/ecs)         # the data-oriented world
#   add_subdirectory(engine/render)      # render graph + high-level renderer
#   add_subdirectory(engine/physics)     # rigid bodies, collision
#   add_subdirectory(engine/destruction) # part-based destruction (headline system)

# Tests are wired in brick M0.2 (doctest via Conan):
# if(RIME_BUILD_TESTS)
#     enable_testing()
#     add_subdirectory(tests)
# endif()

message(STATUS "Rime ${PROJECT_VERSION} — Milestone 0. Targets: rime_core, rime_hello.")
