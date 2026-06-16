# Tests for rime::core. rime_add_test (defined in tests/CMakeLists.txt) builds the exe,
# links doctest, and registers it with CTest; we add the module under test on top.
rime_add_test(rime_core_tests
    version_test.cpp
    log_test.cpp
    assert_test.cpp
    profile_test.cpp)

target_link_libraries(rime_core_tests
    PRIVATE rime::core)
