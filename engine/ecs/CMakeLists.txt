# rime_ecs — the data-oriented world (M4).
#
# Entities (generational ids), a reflection-aware component-type registry, and — as M4 grows —
# archetype/SoA chunk storage, queries, and parallel systems on the job system. Depends only on
# rime::core (handles + reflection today; the allocators and the job system as the storage and
# scheduler land). See docs/adr/0018-ecs-storage-model.md for the storage model and
# docs/design/ecs.md for the design.

add_library(rime_ecs STATIC
    src/entity_directory.cpp)

# Namespaced alias so dependents link `rime::ecs`, matching our C++ namespace.
add_library(rime::ecs ALIAS rime_ecs)

# Public headers under include/ are exposed to anything that links rime_ecs.
target_include_directories(rime_ecs
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC because our public headers include core headers: Entity IS a core::Handle, and the component
# registry captures a core reflection TypeInfo. Anything linking rime::ecs therefore also needs core.
target_link_libraries(rime_ecs
    PUBLIC rime::core)

# Project-wide build policy (C++20 + warning set + any sanitizer); defined in the top-level CMakeLists.
rime_apply_common_target_options(rime_ecs)
