# Tests for rime::scene (M9 / m9.2). The `.rscene` writer/reader: a posed, parented world round-trips
# through text bit-exactly, a hand-authored file loads, floats survive to the bit, and malformed /
# unknown-type / stale-hash / dangling-reference input fails cleanly. All GPU-free, so it runs on
# every CI OS. RIME_SCENE_FIXTURE_DIR points the fixture test at the checked-in first-light scene.
rime_add_test(rime_scene_tests
    scene_format_test.cpp)

target_link_libraries(rime_scene_tests
    PRIVATE rime::scene rime::ecs rime::render rime::core fmt::fmt)

target_compile_definitions(rime_scene_tests
    PRIVATE RIME_SCENE_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/samples/07-first-light")
