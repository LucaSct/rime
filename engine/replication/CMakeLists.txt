# rime_replication — ECS state replication over the net transport (M11.3, ADR-0033 §4 + A9/A10).
#
# NetId identity, the reflection-derived wire schema, and the server/client snapshot replicators:
# the layer that turns "an ecs::World changed" into bytes on a net::Session, and back.
#
# Layer-cake position: THE module that depends on both rime::ecs and rime::net, so that neither has
# to depend on the other. This shape is why the module exists at all rather than the code living in
# engine/net — ADR-0033 §7 claimed engine/net depends on ecs, but it never has (amendment A10), and
# m11.2 paid real design cost to keep it that way (the component schema hash crosses into
# NetDriver::Config as an opaque u64 precisely so the module cannot see ecs). Putting replication
# inside engine/net would have undone that and broken guardrail 2's promise that the engine still
# builds with engine/net deleted.
#
# The precedent for this shape already ships: engine/editorhost depends on rime::ecs + rime::stream
# while rime::stream knows nothing of ecs. This is the same arrangement one storey down.
#
# Still a removable feature module itself (guardrail 2): nothing below it depends on it.

add_library(rime_replication STATIC
    src/net_id.cpp
    src/wire_schema.cpp
    src/snapshot.cpp
    src/server_replicator.cpp
    src/client_replicator.cpp
    src/relevancy.cpp)

# Namespaced alias so dependents link `rime::replication`, matching our C++ namespace.
add_library(rime::replication ALIAS rime_replication)

target_include_directories(rime_replication
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Both PUBLIC: the public headers expose ecs::World/ComponentId and net::Session in their signatures.
target_link_libraries(rime_replication
    PUBLIC rime::ecs rime::net)

# Project-wide build policy (C++20 + warnings-as-errors + any sanitizer); from the top-level CMake.
rime_apply_common_target_options(rime_replication)

# SDK install/export (M6.8): headers + archive into the rime-targets export set.
rime_install_module(rime_replication)
