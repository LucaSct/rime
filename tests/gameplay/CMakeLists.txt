# Tests for rime::gameplay (m12.2). rime_add_test (defined in tests/CMakeLists.txt) builds the exe,
# links doctest, applies the project build policy, and registers it with CTest.
#
# The controller is pure CPU and GPU-free, like the physics it rides on, so this suite runs on every
# CI OS plus ASan/UBSan and TSan. Every case drives the PUBLIC seams only — gameplay::step_character
# and physics::PhysicsWorld — with geometry made of static boxes; nothing here includes anything
# under engine/*/src.
rime_add_test(rime_gameplay_tests
    character_slope_test.cpp
    character_step_test.cpp)

target_link_libraries(rime_gameplay_tests
    PRIVATE rime::gameplay rime::physics)

# The shared fixture header (character_fixture.hpp) lives beside these sources.
target_include_directories(rime_gameplay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
