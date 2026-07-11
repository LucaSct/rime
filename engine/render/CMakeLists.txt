# rime_render — the high-level renderer (M5): the render graph (M5.4), the scene layer — the
# graduated orbit camera, mesh/material registries, the ECS render components (M5.5) — and the
# forward-PBR pass library + scene renderer (M5.6, ADR-0022).
#
# The graph core depends only on rime::rhi (which brings rime::core transitively); the scene
# layer adds rime::ecs (components live in a World). Removable per the modularity guardrail:
# nothing below the samples links it. See docs/adr/0019-render-graph.md and
# docs/design/render-graph.md.

add_library(rime_render STATIC
    src/render_graph.cpp
    src/mesh.cpp
    src/passes.cpp
    src/scene_renderer.cpp
    src/gpu_asset_bridge.cpp)

# The engine's own shaders (M5.6): compiled to SPIR-V at build time and embedded into the
# library, the same offline model the tests/samples use (ADR-0008). The generated headers are
# PRIVATE — consumers see pass objects, never shader bytes.
rime_add_shaders(rime_render
    shaders/depth_only.vert
    shaders/pbr_forward.vert
    shaders/pbr_forward.frag
    shaders/fullscreen.vert
    shaders/tonemap.frag)

# Namespaced alias so dependents link `rime::render`, matching our C++ namespace.
add_library(rime::render ALIAS rime_render)

# Public headers under include/ are exposed to anything that links rime_render.
target_include_directories(rime_render
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# PUBLIC: render_graph.hpp names rhi types (handles, formats, the command-buffer interface) and
# components.hpp names ecs::World, so consumers of rime::render need both sets of headers. rime::assets
# is PUBLIC too: the GPU asset bridge (gpu_asset_bridge.hpp) exposes assets::AssetServer /
# TextureAssetHandle in its interface. This is the ONE allowed assets↔render edge — render *consumes*
# cooked assets and owns their GPU residency; assets never depends on render (ADR-0025).
target_link_libraries(rime_render
    PUBLIC rime::rhi rime::ecs rime::assets)

# Project-wide build policy (C++20 + warning set + any sanitizer); defined in the top-level CMakeLists.
rime_apply_common_target_options(rime_render)

# SDK install/export (M6.8): headers + archive into the rime-targets export set. The embedded
# shader headers stay PRIVATE (rime_add_shaders above), so no shader bytes leak into the SDK.
rime_install_module(rime_render)
