# Tests for viewer-side render math. rime_add_test (tests/CMakeLists.txt) builds the exe, links
# doctest, applies the build policy, and registers it with CTest. The orbit camera GRADUATED to
# engine/render at M5.5 (ADR-0016 rule 3), so its test now links rime::render; the STL loader is
# still viewer-owned, so that sample directory stays on the include path.
rime_add_test(rime_viewer_tests
    camera_test.cpp
    cooked_dogfood_test.cpp
    stl_test.cpp)

# rime::assets + rime::platform join for the M6.6 dogfood: cooked_dogfood_test loads a cooked STL
# (read_mesh) and reads fixture bytes (read_file), then compares against the viewer's live loader.
target_link_libraries(rime_viewer_tests
    PRIVATE rime::core rime::render rime::assets rime::platform)

target_include_directories(rime_viewer_tests
    PRIVATE ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer)

# The dogfood test reads the committed cross-language STL fixtures (cube.stl + cube.rmesh); hand it the
# same fixtures directory the engine/assets tests use (CMake emits forward slashes on every OS).
target_compile_definitions(rime_viewer_tests
    PRIVATE RIME_ASSETS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/assets/fixtures")
