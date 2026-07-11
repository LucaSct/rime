# rime_capi — the engine's C ABI (M6.9), the ADR-0001 stable boundary made real.
#
# This is the ONE SHARED library in the build (everything else is a static module). It exposes a tiny
# extern "C" surface (rime/capi/rime.h) so non-C++ hosts — the Rust editor/tools, tools/rime-ffi —
# can drive the engine without touching a C++ symbol. It links the engine's static modules PRIVATE:
# a SHARED library resolves those archives' symbols INTO the .so, so nothing about rime::app /
# rime::assets appears in rime_capi's link interface — a consumer of the .so needs only the .so.
# (The project-wide PIC + hidden-visibility settings in the top-level CMakeLists are what make that
# static→shared link work and keep the exported symbols to just the C functions.)
add_library(rime_capi SHARED src/capi.cpp)

# Namespaced alias so dependents link rime::capi (matching the other modules).
add_library(rime::capi ALIAS rime_capi)

# Public header: the C ABI, split so it resolves from the source tree in-build and <prefix>/include
# once installed (M6.8's SDK install carries the C face too).
target_include_directories(rime_capi
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# RIME_CAPI_BUILD flips the header's export macro to dllexport (Windows); elsewhere the marked
# functions carry visibility("default") against the module's hidden default.
target_compile_definitions(rime_capi PRIVATE RIME_CAPI_BUILD)

# PRIVATE, and that matters: the C ABI is the whole interface, so these dependencies are an
# implementation detail folded into the .so, not something a consumer of rime::capi links against.
# rime::app brings core/ecs/platform/render/rhi transitively; rime::assets brings the RMA1 reader.
target_link_libraries(rime_capi PRIVATE rime::app rime::assets)

# Project-wide build policy (C++20 + the strong warning set), same as every module.
rime_apply_common_target_options(rime_capi)

# SDK install/export (M6.8): the C face installs with the rest of the SDK, so a downstream project
# gets rime::capi (the .so + rime/capi/rime.h) from find_package(rime) exactly like the C++ modules.
rime_install_module(rime_capi)
