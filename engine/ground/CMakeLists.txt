# rime_ground — the one answer to "what is the ground?" (m17.8, ADR-0041 Ruling 5).
#
# WHY A MODULE RATHER THAN A HELPER. Before this, the ground was five independent numbers in five
# places and no two agreed: in 99-the-block alone one surface was drawn at half-extent 38, collided
# at 44 and GI-traced at 26 x 14, so a player could stand six metres past the visible edge of the
# world. Ruling 5's requirement is "one place that answers 'what is the ground', producing both the
# drawn surface and the collider", shared by two samples and the editor — and in this repository
# `samples/` has no shared code, so "one place" IS a linkable library.
#
# Layer-cake position: above physics and render, below blockkit, the samples and worldkit. It cannot
# live in `worldkit` (that is the component profile and the top of the cake — nothing may depend on
# it), nor in `blockkit` (the vision demo's CONTENT, deliberately physics-free and not installed, so
# 13-networked-player would have to link the block to get a floor), nor in `physics` or `render`
# (neither may see the other, and this owns both halves). Same guardrail-2 argument that produced
# `destruction_render`: nothing in engine/ depends on this one, so deleting the directory leaves the
# engine building — only `worldkit`'s profile names it, loudly, at the one place that has to know.
#
# NOT A TERRAIN MODULE. Ruling 5 defers heightfields, LOD, splat blending and streaming to M18,
# partly because virtualized geometry may subsume terrain LOD. This is named `ground` so that
# question stays open: M18 either grows a heightfield inside `derive_mesh`/`derive_collider` or
# supersedes the module, and either way the seam below it survives.
add_library(rime_ground STATIC
    src/bind.cpp
    src/derive.cpp)

add_library(rime::ground ALIAS rime_ground)

target_include_directories(rime_ground
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# PUBLIC on all four: the headers name GroundSurface's reflection (core), ecs::World, physics's
# ShapeDesc/BodyId and render's CpuMesh/MeshRegistry.
target_link_libraries(rime_ground
    PUBLIC rime::core rime::ecs rime::physics rime::render)

rime_apply_common_target_options(rime_ground)

rime_install_module(rime_ground)
