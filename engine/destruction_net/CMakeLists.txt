# rime_destruction_net — networked destruction (M11.4, ADR-0033 A1/A3/A11).
#
# The damage-op stream: the server publishes the canonical, committed damage-op list on the
# reliable-ordered channel, addressed by the NetIds m11.3 already assigns; clients apply it and never
# run their own contact→damage conversion for the instances it covers.
#
# Layer-cake position: THE module that depends on both rime::destruction and rime::replication, so
# that neither depends on the other. This is the same shape — and the same reasoning — that put ECS
# replication in its own module at m11.3 rather than inside engine/net (ADR-0033 A10). Folding this
# code into rime::replication would have given replication a hard dependency on destruction, and
# guardrail 2 promises the engine still builds with a feature module deleted: removing
# engine/destruction would then have taken the entire networking stack down with it. Folding it into
# rime::destruction is worse still — the headline gameplay system would depend on the transport.
#
# Both of its parents stay removable, and so does it: nothing below depends on this module.

add_library(rime_destruction_net STATIC
    src/destruction_server.cpp
    src/destruction_client.cpp
    src/composition.cpp)

# Namespaced alias so dependents link `rime::destruction_net`, matching our C++ namespace.
add_library(rime::destruction_net ALIAS rime_destruction_net)

target_include_directories(rime_destruction_net
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Both PUBLIC: the public headers expose destruction::DestructionWorld, replication::NetIdMap and
# net::NetDriver in their signatures.
target_link_libraries(rime_destruction_net
    PUBLIC rime::destruction rime::replication)

# Project-wide build policy (C++20 + warnings-as-errors + any sanitizer); from the top-level CMake.
rime_apply_common_target_options(rime_destruction_net)

# SDK install/export (M6.8): headers + archive into the rime-targets export set.
rime_install_module(rime_destruction_net)
