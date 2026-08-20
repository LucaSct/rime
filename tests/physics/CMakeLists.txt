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
    shape_cast_test.cpp
    gjk_test.cpp
    push_in_test.cpp
    events_test.cpp
    ccd_test.cpp
    hull_test.cpp
    compound_test.cpp
    unregister_test.cpp
    stats_test.cpp)

target_link_libraries(rime_physics_tests
    PRIVATE rime::physics)

# gjk_test.cpp is the ONE test that reaches below the PhysicsWorld seam, into engine/physics/src.
# The exception is deliberate and argued at the top of that file: the defect it gates lives in a
# private header, and the layers above it mask the symptom on x86-64 — a 21,000-case sweep through
# the public seam gives byte-identical results with the bug present and absent — so a seam-level
# regression test for it provably cannot exist. Everything else in this suite drives PhysicsWorld
# only, and should keep doing so.
target_include_directories(rime_physics_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/physics/src)
