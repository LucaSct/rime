# rime_vfx — the destruction dust STUB (M8.4).
#
# A small, deletable CPU particle field that turns destruction events into visible feedback (a puff
# of dust at each break). GPU-free by construction — the simulation lives here (spawn/drift/age,
# capped and deterministic), the actual additive draw + its coverage-delta pixel proof land with the
# M8.6 sample, and the real GPU FX system (track fx1) replaces this whole module. It is NOT wired to
# destruction: the fan-out glue (event → emit_burst) lives in the consumer, so the dependency arrow
# never points from vfx into destruction.
#
# Layer-cake position: depends on core only, and NOTHING depends on it — a removable feature module
# (guardrail 2): the engine still builds with it gone.

add_library(rime_vfx STATIC
    src/dust.cpp)

# Namespaced alias so dependents link `rime::vfx`, matching our C++ namespace.
add_library(rime::vfx ALIAS rime_vfx)

target_include_directories(rime_vfx
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# PUBLIC because the seam header (dust.hpp) exposes core::Vec3 in the particle + emit_burst API.
target_link_libraries(rime_vfx
    PUBLIC rime::core)

# Project-wide build policy (C++20 + warnings-as-errors + any sanitizer); from the top-level CMake.
rime_apply_common_target_options(rime_vfx)

# SDK install/export (M6.8): headers + archive into the rime-targets export set.
rime_install_module(rime_vfx)
