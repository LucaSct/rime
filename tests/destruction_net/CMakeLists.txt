# Tests for rime::destruction_net (M11.4a). GPU-free by construction — cooked geometry in, physics
# bodies and packets out — so this suite runs on every CI OS plus ASan/UBSan and TSan, no device
# needed. The network is a ScriptedNetwork on a virtual clock: loss and latency are inputs, so a
# failure is always reproducible and never environment luck.
rime_add_test(rime_destruction_net_tests
    destruction_net_test.cpp)

# rime::destruction_net PUBLIC-links destruction + replication, which transitively brings physics,
# assets, ecs and net — the whole seam this proof drives.
target_link_libraries(rime_destruction_net_tests
    PRIVATE rime::destruction_net)

# The proof stands real walls from the committed m8.1 fixture, shared with the destruction suite and
# the assets oracle (CMake emits forward slashes even on Windows).
target_compile_definitions(rime_destruction_net_tests
    PRIVATE RIME_DESTRUCTION_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/assets/fixtures")
