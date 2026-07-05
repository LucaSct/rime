# rime_render — the high-level renderer (M5): the render graph (M5.4), the scene layer — the
# graduated orbit camera, mesh/material registries, the ECS render components (M5.5) — and next
# the PBR pass library (M5.6).
#
# The graph core depends only on rime::rhi (which brings rime::core transitively); the scene
# layer adds rime::ecs (components live in a World). Removable per the modularity guardrail:
# nothing below the samples links it. See docs/adr/0019-render-graph.md and
# docs/design/render-graph.md.

add_library(rime_render STATIC
    src/render_graph.cpp
    src/mesh.cpp)

# Namespaced alias so dependents link `rime::render`, matching our C++ namespace.
add_library(rime::render ALIAS rime_render)

# Public headers under include/ are exposed to anything that links rime_render.
target_include_directories(rime_render
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC: render_graph.hpp names rhi types (handles, formats, the command-buffer interface) and
# components.hpp names ecs::World, so consumers of rime::render need both sets of headers.
target_link_libraries(rime_render
    PUBLIC rime::rhi rime::ecs)

# Project-wide build policy (C++20 + warning set + any sanitizer); defined in the top-level CMakeLists.
rime_apply_common_target_options(rime_render)
