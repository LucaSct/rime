# Tests for rime::physics (M7). rime_add_test (defined in tests/CMakeLists.txt) builds the exe, links
# doctest, applies the project build policy, and registers it with CTest. The physics core is pure
# CPU, so this suite runs on every CI OS + ASan/UBSan + TSan (the scheduler/solver bricks add the
# threading surface TSan nets).
rime_add_test(rime_physics_tests
    integration_test.cpp
    components_test.cpp
    broadphase_test.cpp
    narrowphase_test.cpp
    solver_test.cpp
    islands_test.cpp
    sync_test.cpp
    query_test.cpp
    events_test.cpp
    ccd_test.cpp)

target_link_libraries(rime_physics_tests
    PRIVATE rime::physics)
