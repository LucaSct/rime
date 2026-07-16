# rime_destruction — the part-based destruction runtime (M8, ADR-0029).
#
# The headline system: it turns a cooked fracture pattern (rime::assets Destructible, M8.1) into
# standing physics — a pattern registered ONCE as convex hulls + a compound (rime::physics,
# ADR-0027/0028), instanced as static compound bodies with per-part state. Damage/connectivity/the
# fracture body-swap (M8.3), the event fan-out (M8.4), and lifetime/budgets (M8.5) grow this module;
# M8.2 is "load, stand, bind". GPU-free by construction — geometry enters as the cook's point/index
# spans and leaves as physics bodies — so every proof runs on lavapipe/CI.
#
# Layer-cake position: it depends on core, ecs, physics, and assets, and NOTHING depends on it — it
# is a removable feature module (guardrail 2): the engine still builds with it gone.

add_library(rime_destruction STATIC
    src/world.cpp
    src/damage.cpp
    src/lifecycle.cpp)

# Namespaced alias so dependents link `rime::destruction`, matching our C++ namespace.
add_library(rime::destruction ALIAS rime_destruction)

# Public headers are the seam; src/ is private (later bricks put the damage/connectivity internals
# there).
target_include_directories(rime_destruction
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# PUBLIC because the seam headers expose assets::DestructibleAsset, physics::BodyId, and the ECS
# components (ecs::World), all of which a dependent sees through rime/destruction/*.hpp.
target_link_libraries(rime_destruction
    PUBLIC rime::core rime::ecs rime::physics rime::assets)

# Project-wide build policy (C++20 + warnings-as-errors + any sanitizer); from the top-level CMake.
rime_apply_common_target_options(rime_destruction)

# SDK install/export (M6.8): headers + archive into the rime-targets export set.
rime_install_module(rime_destruction)
