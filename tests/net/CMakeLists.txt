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
