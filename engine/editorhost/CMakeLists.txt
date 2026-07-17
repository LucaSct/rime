# rime_editorhost — the engine-side editor host (M9 / m9.1, ADR-0031).
#
# The editor is a *client of a live engine* (ADR-0016): this module serves the editor channel against
# a running World over the S1 ProtocolConnection. It is reflection-driven — a component registered
# once (RIME_REFLECT) is snapshot, streamed, and edited with no code here — so it depends on ecs (the
# world + registry), core (reflection + the byte cursor), and stream (the protocol). It is a
# removable tooling module (guardrail 2): nothing in the runtime engine depends on it.

add_library(rime_editorhost STATIC
    src/editor_host.cpp)

# Namespaced alias so dependents link `rime::editorhost`, matching our C++ namespace.
add_library(rime::editorhost ALIAS rime_editorhost)

target_include_directories(rime_editorhost
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# PUBLIC: the header exposes ecs::Entity + stream::ProtocolConnection in its signatures; core comes
# transitively (byte cursor, reflection).
target_link_libraries(rime_editorhost
    PUBLIC rime::ecs rime::stream
    PRIVATE rime::core)

# Project-wide build policy (C++20 + warnings-as-errors + any sanitizer); from the top-level CMake.
rime_apply_common_target_options(rime_editorhost)

# SDK install/export (M6.8): headers + archive into the rime-targets export set.
rime_install_module(rime_editorhost)
