# rime_net — networking (M11, ADR-0033).
#
# Born at m11.1 with the transport v2 foundation: the Link datagram seam (UdpLink over
# platform::UdpSocket + the deterministic ScriptedNetwork proof harness) and the reliability layer
# (ReliableChannel: reliable-ordered + unreliable-sequenced over one socket). Sessions (m11.2),
# replication (m11.3+), and networked destruction (m11.4) build on this.
#
# Layer-cake position: depends on core + platform (sockets) and NOTHING above it — a removable
# feature module (guardrail 2): the engine still builds with it gone.

add_library(rime_net STATIC
    src/link.cpp
    src/reliable_channel.cpp
    src/control_packets.cpp
    src/session.cpp
    src/net_driver.cpp)

# Namespaced alias so dependents link `rime::net`, matching our C++ namespace.
add_library(rime::net ALIAS rime_net)

target_include_directories(rime_net
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# PUBLIC because the public headers expose platform::Endpoint in their signatures.
target_link_libraries(rime_net
    PUBLIC rime::platform)

# Project-wide build policy (C++20 + warnings-as-errors + any sanitizer); from the top-level CMake.
rime_apply_common_target_options(rime_net)

# SDK install/export (M6.8): headers + archive into the rime-targets export set.
rime_install_module(rime_net)
