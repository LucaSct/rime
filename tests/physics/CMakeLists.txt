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
    penetration_test.cpp
    shape_cast_test.cpp
    gjk_test.cpp
    separated_hull_test.cpp
    push_in_test.cpp
    events_test.cpp
    ccd_test.cpp
    hull_test.cpp
    heightfield_test.cpp
    compound_test.cpp
    plain_bounds_test.cpp
    contact_capture_test.cpp
    contact_workload_test.cpp
    contact_profile_test.cpp
    unregister_test.cpp
    stats_test.cpp)

target_link_libraries(rime_physics_tests
    PRIVATE rime::physics)

# gjk_test.cpp reaches below PhysicsWorld to pin arithmetic masked by the public seam on x86-64
# (the 21,000-case public sweep was byte-identical with and without that defect). The separated
# hull fixture also needs exact runtime bits: re-registering its geometry re-centres the vertices.
# Each file explains its exception. solver_test.cpp uses the same access for bitwise equivalence
# of the velocity-phase inertia cache's two overloads. Other proofs should drive PhysicsWorld.
# Contact replay additionally needs the unfiltered private dispatch as an independent oracle;
# otherwise the bounds sweep could compare the optimized path with itself. Cache lifetime and
# CCD are separately pinned through the public world seam in plain_bounds_test.cpp.
target_include_directories(rime_physics_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/physics/src)

# The heightfield seam rule is unreachable by ordinary rays, so the test that pins it must SEE it
# fire (a counter in heightfield.hpp, compiled in only under this macro and only for this target --
# the library and every other build keep the plain hot path). See the hook's comment there.
target_compile_definitions(rime_physics_tests PRIVATE RIME_PHYSICS_SEAM_COUNTER)
