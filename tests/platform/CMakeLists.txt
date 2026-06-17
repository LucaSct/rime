# Tests for rime::platform. rime_add_test (defined in tests/CMakeLists.txt) builds the exe,
# links doctest, applies the project build policy, and registers it with CTest; we add the
# module under test on top.
rime_add_test(rime_platform_tests
    clock_test.cpp
    threads_test.cpp
    init_test.cpp
    event_test.cpp
    window_test.cpp
    input_test.cpp
    filesystem_test.cpp)

target_link_libraries(rime_platform_tests
    PRIVATE rime::platform)
