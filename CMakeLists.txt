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

# Position-independent code everywhere. The C ABI (engine/capi, M6.9) is a SHARED library that links
# the engine's STATIC module libraries, and linking a static archive into a shared object requires
# its objects to be PIC. The runtime cost is negligible here, and it readies the engine for other
# shared faces later. (Static linking into tests/samples/the SDK consumer is unaffected.)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Hidden symbol visibility by default. The only symbols the rime_capi shared library exports are the
# handful marked RIME_CAPI_API (visibility "default") in rime/capi/rime.h — the C++ innards of the
# linked static libs stay internal to the .so, keeping the ABI surface exactly the C functions we
# publish. Visibility does not affect STATIC linking (archive symbols still resolve into executables
# and the installed static libs), so this is free for the rest of the build.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

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
option(RIME_BUILD_CAPI "Build the rime_capi C ABI shared library (M6.9)" ON)
option(RIME_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# ── Sanitizers (Phase 0.3) ────────────────────────────────────────────────────
# A build-wide switch for the Clang/GCC sanitizers, off by default. `address` enables
# AddressSanitizer + UndefinedBehaviorSanitizer together (ASan finds use-after-free / OOB /
# leaks; UBSan finds signed overflow, bad shifts, misaligned/invalid pointers, …). `thread`
# enables ThreadSanitizer — the one that matters most to us, because it is how we keep the
# hand-rolled lock-free Chase-Lev deque (engine/core/.../jobs/chase_lev_deque.hpp) and the
# job system honest under real multi-threaded load, on weak-memory hardware CI can't provide.
# ASan and TSan are mutually exclusive (they cannot coexist in one process), so this is a
# single choice, not a set. Sanitizers are a GCC/Clang toolchain feature; on MSVC a non-off
# value is rejected rather than silently ignored — the sanitizer CI jobs run on Linux. The
# flags are validated + computed once here and applied to every target's compile AND link
# lines in rime_apply_common_target_options() below (the single funnel all our targets use).
set(RIME_SANITIZER "off" CACHE STRING "Build with a sanitizer: off | address (ASan+UBSan) | thread (TSan)")
set_property(CACHE RIME_SANITIZER PROPERTY STRINGS off address thread)

if(NOT RIME_SANITIZER MATCHES "^(off|address|thread)$")
    message(FATAL_ERROR "RIME_SANITIZER must be one of: off | address | thread (got '${RIME_SANITIZER}').")
endif()
if(RIME_SANITIZER STREQUAL "off")
    # No instrumentation; RIME_SANITIZER_FLAGS stays empty and the funnel below is a no-op.
elseif(MSVC)
    message(FATAL_ERROR
        "RIME_SANITIZER=${RIME_SANITIZER} needs a GCC/Clang toolchain (the sanitizer CI jobs "
        "run on Linux). Configure MSVC builds with -DRIME_SANITIZER=off.")
elseif(RIME_SANITIZER STREQUAL "address")
    # Pair ASan with UBSan: one build covers both, and they share the frame-pointer/stack-trace
    # machinery. -fno-sanitize-recover=all makes any UBSan finding *abort* (a nonzero exit CI
    # catches) instead of printing and continuing; -fno-omit-frame-pointer + -g keep every
    # report's file:line readable even in optimized builds.
    set(RIME_SANITIZER_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all
                             -fno-omit-frame-pointer -g)
else() # thread
    set(RIME_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer -g)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GCC's ThreadSanitizer cannot instrument a standalone std::atomic_thread_fence — it
        # emits -Wtsan and treats the fence as a no-op. But the Chase-Lev deque's correctness
        # rests on exactly those fences (the seq-cst StoreLoad barrier in pop()/steal(), per
        # Lê/Pop/Cohen/Nardelli 2013). -Werror would turn that advisory into a hard build
        # failure, so we downgrade it — but GCC's TSan then *under-checks* the deque (it can't
        # see the happens-before the fences establish). Clang's TSan does model
        # atomic_thread_fence, so use it for a meaningful run — which is why the CI TSan job
        # runs on Clang. See docs/design/work-stealing-deque.md.
        list(APPEND RIME_SANITIZER_FLAGS -Wno-tsan)
        message(WARNING
            "RIME_SANITIZER=thread with GCC: GCC's ThreadSanitizer cannot model the standalone "
            "atomic_thread_fence()s the lock-free deque is built on, so it under-checks them. "
            "Use Clang (CC=clang CXX=clang++) for a meaningful TSan run — the CI TSan job does.")
    endif()
endif()

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

    # Sanitizer instrumentation (see RIME_SANITIZER above). The flags must reach both the
    # compile line (to instrument this target's objects) and the link line (so the executable
    # pulls in the sanitizer runtime), so we add both. RIME_SANITIZER_FLAGS is empty when the
    # switch is off, making this a no-op for normal builds.
    if(RIME_SANITIZER_FLAGS)
        target_compile_options(${target} PRIVATE ${RIME_SANITIZER_FLAGS})
        target_link_options(${target} PRIVATE ${RIME_SANITIZER_FLAGS})
    endif()
endfunction()

# ── SDK install / export (M6.8, ADR-0016 rule 5) ──────────────────────────────
# `cmake --install` lays down a usable Rime SDK: public headers + the static libs + a CMake
# package config, so an out-of-tree app does `find_package(rime CONFIG)` +
# `target_link_libraries(app rime::render …)` and builds. See docs/design/sdk.md and the
# out-of-tree proof in tests/sdk_consumer/. GNUInstallDirs gives the conventional per-platform
# lib/, include/, bin/ layout; CMakePackageConfigHelpers writes the config + version files.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Where the package config + the exported targets file land in the install tree.
set(RIME_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/rime")

# rime_install_module(<target>): register one engine-module library into the shared
# `rime-targets` export set and install its public headers. Every module funnels through here
# exactly as it funnels through rime_apply_common_target_options() — one place that defines what
# "install a Rime module" means. Modules must already give their PUBLIC include dir the
# BUILD_INTERFACE/INSTALL_INTERFACE split (so a header resolves from the source tree in-build and
# from <prefix>/include once installed — that $<INSTALL_INTERFACE:> already records the install
# location, so we don't also pass INCLUDES DESTINATION here and duplicate it). A CMake function does
# NOT open a new directory scope, so CMAKE_CURRENT_SOURCE_DIR here is the *calling* module's dir —
# which is what makes the per-module header install work.
function(rime_install_module target)
    # The real target is `rime_core`; strip the prefix so that with the rime:: export NAMESPACE the
    # consumer links `rime::core` — the SAME name as the in-tree add_library(rime::core ALIAS …)
    # (aliases are not themselves exported, so without EXPORT_NAME the export would emit the raw
    # `rime::rime_core`, and code could not move between an in-tree and an installed build unedited).
    string(REGEX REPLACE "^rime_" "" _export_name "${target}")
    set_target_properties(${target} PROPERTIES EXPORT_NAME ${_export_name})
    install(TARGETS ${target}
        EXPORT rime-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endfunction()

# rime_install_support_lib(<target>): register an internal, header-less static helper into the
# same export set. These (rime_vma, rime_xdg_shell) are PRIVATE-linked into a module, but a STATIC
# link turns even a PRIVATE dependency into a link-time requirement the final consumer executable
# must satisfy — so the archive has to be installed and exported too, or the consumer link fails
# with unresolved symbols. No public headers: their include dirs are BUILD_INTERFACE-only.
function(rime_install_support_lib target)
    # Same EXPORT_NAME de-prefixing as the modules (cosmetic here — consumers pull these in
    # transitively via rime::rhi / rime::platform and never name them directly).
    string(REGEX REPLACE "^rime_" "" _export_name "${target}")
    set_target_properties(${target} PROPERTIES EXPORT_NAME ${_export_name})
    install(TARGETS ${target}
        EXPORT rime-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
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

# Streaming codec dependencies (Track S / S0.3), found once here so the imported targets are visible
# to engine/stream below. libjpeg-turbo provides the TurboJPEG API (the lossy wire codec); lz4 the
# lossless one — chosen by measurement in ADR-0017. Both are *shipped* (engine/stream is a
# shippable feature) and both are permissive (BSD), per third_party/README.md. Their component target
# names carry the link kind (…-static for our default static build), so pick whichever exists to
# avoid hard-coding static vs. shared. See conanfile.py.
find_package(libjpeg-turbo REQUIRED) # libjpeg-turbo::turbojpeg-static (or ::turbojpeg if shared)
find_package(lz4 REQUIRED)           # LZ4::lz4_static (or LZ4::lz4_shared if shared)

if(TARGET libjpeg-turbo::turbojpeg-static)
    set(RIME_TURBOJPEG_TARGET libjpeg-turbo::turbojpeg-static)
else()
    set(RIME_TURBOJPEG_TARGET libjpeg-turbo::turbojpeg)
endif()
if(TARGET LZ4::lz4_static)
    set(RIME_LZ4_TARGET LZ4::lz4_static)
else()
    set(RIME_LZ4_TARGET LZ4::lz4_shared)
endif()

# Inter-frame video codec (Track S / s1.2): SVT-AV1 encodes, dav1d decodes — the AV1 pair
# ADR-0030 chose on licensing (royalty-free by design; H.264/HEVC's patent pools are the same
# class of shipping trap as x264's GPL). Both ship, both permissive (BSD + the AOM patent
# license), both linked PRIVATE into engine/stream behind opaque handles like the codecs above.
find_package(libsvtav1 REQUIRED) # libsvtav1::encoder (the SvtAv1Enc component)
find_package(dav1d REQUIRED)     # dav1d::dav1d
set(RIME_SVTAV1_TARGET libsvtav1::encoder)
set(RIME_DAV1D_TARGET dav1d::dav1d)

# ── Modules ───────────────────────────────────────────────────────────────────
# Added bottom-up as each is implemented. The dependency order mirrors the layer cake
# in docs/ARCHITECTURE.md (a module may only depend on layers below it).
add_subdirectory(engine/core)     # foundation: version (M0); memory, math, jobs… (M1+)
add_subdirectory(engine/platform) # OS abstraction: window, input, files, threads (M2+)
add_subdirectory(engine/rhi)      # Render Hardware Interface + Vulkan backend (M3)
add_subdirectory(engine/ecs)      # the data-oriented world: entities, components, systems (M4)
add_subdirectory(engine/physics)  # own rigid-body core: bodies, collision, queries (M7, ADR-0026)
add_subdirectory(engine/assets)   # cooked-asset loading: RMA1 reader, registry, manifest (M6)
add_subdirectory(engine/destruction) # part-based destruction: patterns, instances, fracture (M8, ADR-0029)
add_subdirectory(engine/audio)    # audio seam: play(sound, position, gain) + null backend (M8.4)
add_subdirectory(engine/vfx)      # destruction dust stub: CPU billboard particles (M8.4, fx1 replaces)
add_subdirectory(engine/render)   # render graph + high-level renderer (M5)
add_subdirectory(engine/stream)   # graphics streaming: frame capture → encode → transport (Track S)
add_subdirectory(engine/editorhost) # editor-as-client host: serve the world over the channel (M9, ADR-0031)
add_subdirectory(engine/scene)    # .rscene text scene format: reflection-serialized world save/load (M9, ADR-0031)
add_subdirectory(engine/app)      # launcher stub (M0) → application framework (later)

# The C ABI (M6.9): a SHARED library over a tiny extern "C" surface — the ADR-0001 boundary the Rust
# editor/tools drive the engine through. Sits on top of app+assets; optional per the modularity
# guardrail (the engine still builds without it).
if(RIME_BUILD_CAPI)
    add_subdirectory(engine/capi)
endif()

# Not yet wired — they land in later milestones/bricks (see docs/ROADMAP.md):
#   add_subdirectory(engine/physics)     # rigid bodies, collision
#   add_subdirectory(engine/destruction) # part-based destruction (headline system)

# Turn on CTest BEFORE any subdirectory that registers tests. Samples register their headless
# self-checks as smoke tests (add_test), and add_test only takes effect once enable_testing() has
# run — so it must precede the samples subdir, not just the tests one.
if(RIME_BUILD_TESTS)
    enable_testing()
endif()

# ── Samples ─────────────────────────────────────────────────────────────────────
# Runnable example projects that exercise the engine; each milestone's proof lives here.
# First up: M1.6's job-system core-saturation demo. Some samples also register a headless
# self-check as a CTest smoke test (guarded on a device — a skip where none exists).
if(RIME_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

# ── Tests ───────────────────────────────────────────────────────────────────────
# Unit tests live under tests/ (one target per module; see docs/ROADMAP.md, "Testing &
# profiling"). doctest is resolved by Conan as a test-only dependency, so we only look
# for it when tests are enabled. CTest was enabled above so `ctest --preset dev` drives it.
if(RIME_BUILD_TESTS)
    find_package(doctest REQUIRED)
    add_subdirectory(tests)
endif()

# ── Install the export set + package config (M6.8) ────────────────────────────
# Each rime_install_module()/rime_install_support_lib() above registered its target into the
# `rime-targets` export set; here we write that set out and generate the package config that
# find_package(rime CONFIG) loads. The rime:: namespace on the export means a consumer sees the
# SAME target names (rime::core, rime::render, …) as an in-tree build — code moves between the two
# without edits. configure_package_config_file expands @PACKAGE_INIT@ and keeps the install
# relocatable (no absolute paths baked in); the version file answers find_package(rime x.y)
# with SameMajorVersion compatibility (pre-1.0 the semver promise is deliberately weak — a VISION
# non-goal, spelled out in docs/design/sdk.md).
install(EXPORT rime-targets
    FILE rime-targets.cmake
    NAMESPACE rime::
    DESTINATION ${RIME_INSTALL_CMAKEDIR})

configure_package_config_file(
    "${RIME_CMAKE_DIR}/rime-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/rime-config.cmake"
    INSTALL_DESTINATION ${RIME_INSTALL_CMAKEDIR})

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/rime-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/rime-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/rime-config-version.cmake"
    DESTINATION ${RIME_INSTALL_CMAKEDIR})

message(STATUS "Rime ${PROJECT_VERSION} — Targets: rime_core, rime_platform, rime_rhi, rime_ecs, rime_physics, rime_assets, rime_destruction, rime_audio, rime_vfx, rime_render, rime_stream, rime_app, rime_capi, rime_hello.")
