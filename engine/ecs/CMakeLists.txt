# rime_ecs — the data-oriented world (M4).
#
# Entities (generational ids), a reflection-aware component-type registry, and — as M4 grows —
# archetype/SoA chunk storage, queries, and parallel systems on the job system. Depends only on
# rime::core (handles + reflection today; the allocators and the job system as the storage and
# scheduler land). See docs/adr/0018-ecs-storage-model.md for the storage model and
# docs/design/ecs.md for the design.

add_library(rime_ecs STATIC
    src/entity_directory.cpp
    src/signature.cpp
    src/chunk_pool.cpp
    src/chunk.cpp
    src/archetype.cpp
    src/world.cpp
    src/schedule.cpp
    src/transform.cpp)

# Namespaced alias so dependents link `rime::ecs`, matching our C++ namespace.
add_library(rime::ecs ALIAS rime_ecs)

# Public headers under include/ are exposed to anything that links rime_ecs.
target_include_directories(rime_ecs
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# PUBLIC because our public headers include core headers: Entity IS a core::Handle, and the component
# registry captures a core reflection TypeInfo. Anything linking rime::ecs therefore also needs core.
target_link_libraries(rime_ecs
    PUBLIC rime::core)

# Project-wide build policy (C++20 + warning set + any sanitizer); defined in the top-level CMakeLists.
rime_apply_common_target_options(rime_ecs)

# SDK install/export (M6.8): headers + archive into the rime-targets export set.
rime_install_module(rime_ecs)
