# rime_render — the high-level renderer (M5): the render graph today, then the scene layer
# (camera, meshes, materials — M5.5) and the PBR pass library (M5.6) on top of it.
#
# The graph core depends only on rime::rhi (which brings rime::core transitively) — it schedules
# and records; it does not know what a scene is. The ECS-facing scene layer joins this module at
# M5.5 and adds the rime::ecs dependency then. Removable per the modularity guardrail: nothing
# below the samples links it yet. See docs/adr/0019-render-graph.md and
# docs/design/render-graph.md.

add_library(rime_render STATIC
    src/render_graph.cpp)

# Namespaced alias so dependents link `rime::render`, matching our C++ namespace.
add_library(rime::render ALIAS rime_render)

# Public headers under include/ are exposed to anything that links rime_render.
target_include_directories(rime_render
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC: render_graph.hpp names rhi types (handles, formats, the command-buffer interface), so
# consumers of rime::render need the rhi headers too.
target_link_libraries(rime_render
    PUBLIC rime::rhi)

# Project-wide build policy (C++20 + warning set + any sanitizer); defined in the top-level CMakeLists.
rime_apply_common_target_options(rime_render)
