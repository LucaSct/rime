# rime_terrain_collision — regional terrain collision with deferred admission (m19.8c, ADR-0068).
#
# Which heightfield tiles are in the physics world, decided at the tick barrier and nowhere else:
# activation and eviction from the simulation's own demands, pins, deferred admission for spawns and
# teleports, and the counted stall that keeps an admitted body off ground that has not loaded.
#
# WHY A MODULE. It is the one place that needs BOTH the asset server's streamed handles and the
# physics world's heightfield store, and neither of those may know the other. Guardrail 2: nothing
# in engine/ depends on this target, so removing the add_subdirectory line leaves the engine
# building — a game with a small fully-resident map simply does not link it.
#
# Depends only on the PUBLIC interfaces of assets and physics. No renderer (what the camera sees
# must not be able to change what the simulation collides with) and no network (activation
# transport is a later brick).
add_library(rime_terrain_collision STATIC
    src/asset_server_source.cpp
    src/terrain_collision.cpp)

add_library(rime::terrain_collision ALIAS rime_terrain_collision)

target_include_directories(rime_terrain_collision
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(rime_terrain_collision
    PUBLIC rime::core rime::assets rime::physics)

rime_apply_common_target_options(rime_terrain_collision)

rime_install_module(rime_terrain_collision)
