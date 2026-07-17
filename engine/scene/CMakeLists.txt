# rime_scene — the `.rscene` scene format (M9 / m9.2, ADR-0031).
#
# A world written to a human-diffable text file and read back, reflection-driven: a component
# registered once (RIME_REFLECT) saves and loads with no code here, keyed by its stable type_hash.
# It is the *reference* implementation of the format — the Rust editor reuses it via files, never by
# reimplementing the byte layout. Depends on ecs (the world + registry + the transform/Entity
# reflection it reads) and core (reflection + fmt). A pure authoring/serialization module: nothing in
# the runtime frame path depends on it, and it needs no GPU.

add_library(rime_scene STATIC
    src/scene_format.cpp)

# Namespaced alias so dependents link `rime::scene`, matching our C++ namespace.
add_library(rime::scene ALIAS rime_scene)

target_include_directories(rime_scene
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# PUBLIC ecs: the header takes ecs::World& in its signatures (core comes transitively — reflection).
# PRIVATE fmt: the writer/error messages format through it, an implementation detail.
target_link_libraries(rime_scene
    PUBLIC rime::ecs
    PRIVATE rime::core fmt::fmt)

# Project-wide build policy (C++20 + warnings-as-errors + any sanitizer); from the top-level CMake.
rime_apply_common_target_options(rime_scene)

# SDK install/export (M6.8): headers + archive into the rime-targets export set.
rime_install_module(rime_scene)
