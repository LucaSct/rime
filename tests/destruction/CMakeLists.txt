# Tests for rime::destruction (M8). rime_add_test (defined in tests/CMakeLists.txt) builds the exe,
# links doctest, applies the project build policy, and registers it with CTest. The destruction
# runtime is pure CPU (cooked geometry → physics bodies), so this suite runs on every CI OS +
# ASan/UBSan + TSan, no device needed.
rime_add_test(rime_destruction_tests
    world_test.cpp
    damage_test.cpp
    components_test.cpp)

# rime::destruction PUBLIC-links core/ecs/physics/assets, so this one link brings the whole seam
# (PhysicsWorld, the cooked-asset reader, the ECS world) transitively.
target_link_libraries(rime_destruction_tests
    PRIVATE rime::destruction)

# The world test loads the committed m8.1 fixture (shared with the assets oracle) — hand it the
# fixtures directory as a string (CMake emits forward slashes even on Windows).
target_compile_definitions(rime_destruction_tests
    PRIVATE RIME_DESTRUCTION_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/assets/fixtures")
