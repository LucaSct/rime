# Tests for rime::net. rime_add_test (defined in tests/CMakeLists.txt) builds the exe,
# links doctest, applies the project build policy, and registers it with CTest.
rime_add_test(rime_net_tests
    reliable_channel_test.cpp)

target_link_libraries(rime_net_tests
    PRIVATE rime::net)
