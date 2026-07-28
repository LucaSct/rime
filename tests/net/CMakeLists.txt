# Tests for rime::net. rime_add_test (defined in tests/CMakeLists.txt) builds the exe,
# links doctest, applies the project build policy, and registers it with CTest.
rime_add_test(rime_net_tests
    reliable_channel_test.cpp)

target_link_libraries(rime_net_tests
    PRIVATE rime::net)

# Its own executable rather than a second file in the one above: each doctest binary owns exactly
# one main(), and separate targets keep `ctest -R` able to run the session proofs alone.
rime_add_test(rime_net_session_tests
    session_test.cpp)

target_link_libraries(rime_net_session_tests
    PRIVATE rime::net)

# The two-process smoke proof (m11.2). NOT rime_add_test: that helper links doctest and registers
# the test with a no-argument command, and this is a standalone program that needs a mode argument.
# CI runs `duo`, which binds an ephemeral port and re-executes itself as the client — one test, one
# result, no port to race and no startup order to manage.
add_executable(rime_net_smoke net_smoke.cpp)

target_link_libraries(rime_net_smoke
    PRIVATE rime::net)

rime_apply_common_target_options(rime_net_smoke)

add_test(NAME rime_net_smoke COMMAND rime_net_smoke duo)

# The `server`/`client` modes of the same binary are the LAN run, and they are deliberately NOT
# registered as a test: CI is a single headless box, so a loopback run is all it can honestly prove.
# Two machines talking to each other is a human gate on the dev server — see engine/net/README.md.
