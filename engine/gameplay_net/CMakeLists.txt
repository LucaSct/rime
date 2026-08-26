# rime_gameplay_net — the networked player (m12.3, ADR-0035 §3/§4).
#
# The server consume loop, the session ↔ avatar registry, the replicated `LastProcessedInput`
# pairing, and the one message that tells a client which NetId is its own. m12.4's Predictor lands
# here too.
#
# Layer-cake position: THE module that depends on both rime::gameplay and rime::replication, so
# that neither depends on the other. This is the destruction_net argument verbatim, and it is worth
# restating because it is the reason two small modules exist instead of one convenient large one:
#
#   * folding this into rime::replication would give the transport a hard dependency on gameplay,
#     so deleting engine/gameplay would take the whole networking stack with it;
#   * folding it into rime::gameplay would make the character controller — which a single-player
#     game wants on its own — depend on sockets;
#   * and both of its parents stay removable, as does it: nothing below depends on this module.
#
# NOTE WHAT IS ALSO NOT HERE: no rime::destruction edge. A shot reports what it hit; turning that
# into damage is the consumer's glue, exactly as the M8.4 fan-out keeps rime::vfx from ever linking
# destruction. That is what lets the same weapon shoot a destructible wall in one game and a health
# bar in another, and it is why `ShotEvent` carries the resolved damage arguments rather than a
# `destruction::DamageOp`.
#
# GPU-free by construction: every proof in tests/gameplay_net runs on any CI machine.

add_library(rime_gameplay_net STATIC
    src/components.cpp
    src/gameplay_client.cpp
    src/gameplay_server.cpp
    src/player_registry.cpp
    src/predictor.cpp)

# Namespaced alias so dependents link `rime::gameplay_net`, matching our C++ namespace.
add_library(rime::gameplay_net ALIAS rime_gameplay_net)

target_include_directories(rime_gameplay_net
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Both PUBLIC: the public headers expose gameplay::CharacterInput/StepStats,
# replication::ServerInputReceiver/NetIdMap and net::NetDriver in their signatures.
target_link_libraries(rime_gameplay_net
    PUBLIC rime::gameplay rime::replication)

# Project-wide build policy (C++20 + warnings-as-errors + any sanitizer); from the top-level CMake.
rime_apply_common_target_options(rime_gameplay_net)

# SDK install/export (M6.8): headers + archive into the rime-targets export set.
rime_install_module(rime_gameplay_net)
