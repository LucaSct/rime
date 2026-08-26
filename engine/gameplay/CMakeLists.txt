# rime_gameplay — the character controller (m12.2, ADR-0035 §3).
#
# How a player-shaped capsule moves through a simulated world WITHOUT being simulated by it: a
# kinematic body whose pose the game computes with shape casts, using collide-and-slide. A dynamic
# rigid body makes a terrible player (it tips over, bounces off stairs, and slides down every ramp),
# so this module is the deliberate alternative — authored motion that stops at walls, slides along
# them, climbs steps, and sticks to slopes.
#
# Layer-cake position: gameplay -> core + ecs + physics, through PUBLIC seams only. Note what is
# NOT here:
#
#   * no rime::destruction edge. The M8.4 fan-out rule: destruction depends on physics, and having
#     gameplay depend on destruction too would make the headline system un-removable from a build
#     that only wants a player.
#   * no rime::net / rime::replication edge. CharacterInput mirrors replication::InputCommand by
#     VALUE SHAPE, not by linkage, precisely so a single-player build can delete the whole
#     networking stack and still have a controller — guardrail 2's promise, kept by not making the
#     link in the first place. gameplay_net (m12.3) is where the two meet.
#
# GPU-free by construction, like physics: every proof in tests/gameplay runs on any CI machine.

add_library(rime_gameplay STATIC
    src/character.cpp
    src/components.cpp
    src/weapon.cpp)

# Namespaced alias so dependents link `rime::gameplay`, matching our C++ namespace.
add_library(rime::gameplay ALIAS rime_gameplay)

target_include_directories(rime_gameplay
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# All PUBLIC: the seam headers expose core math, physics::PhysicsWorld/BodyId, and (in
# components.hpp) the ecs reflection macros.
target_link_libraries(rime_gameplay
    PUBLIC rime::core rime::ecs rime::physics)

# Project-wide build policy (C++20 + warnings-as-errors + any sanitizer); from the top-level CMake.
rime_apply_common_target_options(rime_gameplay)

# SDK install/export (M6.8): headers + archive into the rime-targets export set.
rime_install_module(rime_gameplay)
