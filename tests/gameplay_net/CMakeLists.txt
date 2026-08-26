# Tests for rime::gameplay_net (m12.3). GPU-free by construction — commands in, authoritative
# motion and packets out — so this suite runs on every CI OS plus ASan/UBSan and TSan, no device
# needed. The network is a ScriptedNetwork on a virtual clock: loss and latency are inputs, so a
# failure is always reproducible and never environment luck.
#
# The weapon → destruction glue lives in its own translation unit because it is the one file here
# that links rime::destruction — deliberately, since that is exactly the dependency the ENGINE
# module refuses to take (see engine/gameplay_net/CMakeLists.txt). Proving the glue in the consumer
# is proving it where it actually lives.
rime_add_test(rime_gameplay_net_tests
    consume_loop_test.cpp
    latency_test.cpp
    prediction_test.cpp
    smoothing_test.cpp
    weapon_glue_test.cpp)

# rime::gameplay_net PUBLIC-links gameplay + replication, which transitively brings physics, ecs,
# core and net. rime::destruction is the consumer-side dependency the glue test stands in for.
target_link_libraries(rime_gameplay_net_tests
    PRIVATE rime::gameplay_net rime::destruction)

# The shared harness (match_fixture.hpp) lives beside these sources.
target_include_directories(rime_gameplay_net_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The glue proof stands a real wall from the committed m8.1 fixture, shared with the destruction
# suite and the assets oracle (CMake emits forward slashes even on Windows).
target_compile_definitions(rime_gameplay_net_tests
    PRIVATE RIME_DESTRUCTION_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/assets/fixtures")
