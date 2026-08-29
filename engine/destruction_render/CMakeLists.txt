# rime_destruction_render — per-part render leaves for destructibles (m13.2d).
#
# The bridge that turns a DestructionWorld's parts and debris into entities the renderer draws:
# one mesh per (pattern, part) uploaded once, one leaf entity per (instance, part) posed every tick
# from whichever of the three visual states it is in — standing, riding a debris body, or frozen at
# its last pose — and despawned when m13.2b's visual budget evicts its rubble.
#
# WHY A BRIDGE MODULE. `destruction` must not depend on `render` (it sits below it and is proven
# GPU-free), and `render` must not depend on `destruction` (a renderer that knows what a fracture is
# has lost the seam). So the glue is its own module depending on both — the same guardrail-2
# argument that produced `replication` and `destruction_net`. Nothing depends on this one: delete
# the directory and the engine still builds, the block simply is not drawn.

add_library(rime_destruction_render STATIC
    src/part_leaves.cpp)

add_library(rime::destruction_render ALIAS rime_destruction_render)

target_include_directories(rime_destruction_render
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# PUBLIC on both sides: the header names DestructionWorld/PatternId/InstanceId (destruction, which
# brings physics + assets + ecs transitively) and MeshRegistry/MaterialId (render).
target_link_libraries(rime_destruction_render
    PUBLIC rime::destruction rime::render rime::ecs rime::core)

rime_apply_common_target_options(rime_destruction_render)

rime_install_module(rime_destruction_render)
