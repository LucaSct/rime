# rime_physics — the rigid-body physics core (M7, ADR-0026).
#
# An OWN rigid-body engine, not a wrapper around a third-party solver: the algorithm suite
# (broadphase, GJK/EPA narrowphase, sequential-impulse solver, islands, CCD) is grown here so the
# code teaches and is shaped for destruction. This module is GPU-free by construction — collision
# geometry enters as point/index spans, never GPU resources — so every proof runs on lavapipe/CI.
#
# Layer-cake position: it depends only on core (math, handles, allocators, the job system) and ecs
# (components live here), and NOTHING depends on its internals — the solver/broadphase live under
# src/ (PRIVATE), invisible above the seam. Destruction (M8), render/lighting (M10), and the
# effects/fluids tracks depend on rime::physics, never the reverse.
#
# M7.1 ships the seam + the SoA body pool + integration (no collision yet); later bricks add
# broadphase (M7.2), narrowphase (M7.3), the solver (M7.4), the parallel step (M7.5), etc.

add_library(rime_physics STATIC
    src/world.cpp)

# Namespaced alias so dependents link `rime::physics`, matching our C++ namespace.
add_library(rime::physics ALIAS rime_physics)

# Public headers are the seam; src/ is private (later bricks put broadphase/solver internals there).
target_include_directories(rime_physics
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# PUBLIC because the seam headers use core math/handles and the ECS components include ecs::World.
target_link_libraries(rime_physics
    PUBLIC rime::core rime::ecs)

# Project-wide build policy (C++20 + warnings-as-errors + any sanitizer); from the top-level CMake.
rime_apply_common_target_options(rime_physics)

# SDK install/export (M6.8): headers + archive into the rime-targets export set.
rime_install_module(rime_physics)
