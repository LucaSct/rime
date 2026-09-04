# Tests for rime::core. rime_add_test (defined in tests/CMakeLists.txt) builds the exe,
# links doctest, and registers it with CTest; we add the module under test on top.
rime_add_test(rime_core_tests
    version_test.cpp
    log_test.cpp
    assert_test.cpp
    profile_test.cpp
    work_ledger_test.cpp
    perf_report_test.cpp
    memory_test.cpp
    math_test.cpp
    quat_test.cpp
    transform_test.cpp
    slot_map_test.cpp
    event_channel_test.cpp
    chase_lev_deque_test.cpp
    jobs_test.cpp
    reflect_test.cpp
    hash_test.cpp
    byte_cursor_test.cpp
    module_test.cpp)

# The work-stealing-deque test spins up real threads, so it needs the platform threads library.
find_package(Threads REQUIRED)

target_link_libraries(rime_core_tests
    PRIVATE rime::core Threads::Threads)

# The perf gate's falsification reads a REAL committed report (m17.3c): the point of
# "a rule naming a timeline nobody recorded fails" is weaker if the report it fails on is one the
# test just invented. Pointing it at docs/perf/ also means the day someone renames or deletes a
# baseline, this test says so instead of the next perf run finding out.
target_compile_definitions(rime_core_tests
    PRIVATE RIME_PERF_BASELINE_DIR="${CMAKE_SOURCE_DIR}/docs/perf")
