# Tests for rime::core. rime_add_test (defined in tests/CMakeLists.txt) builds the exe,
# links doctest, and registers it with CTest; we add the module under test on top.
rime_add_test(rime_core_tests
    version_test.cpp
    log_test.cpp
    assert_test.cpp
    profile_test.cpp
    memory_test.cpp
    math_test.cpp
    quat_test.cpp
    transform_test.cpp
    slot_map_test.cpp
    chase_lev_deque_test.cpp
    jobs_test.cpp
    reflect_test.cpp)

# The work-stealing-deque test spins up real threads, so it needs the platform threads library.
find_package(Threads REQUIRED)

target_link_libraries(rime_core_tests
    PRIVATE rime::core Threads::Threads)
