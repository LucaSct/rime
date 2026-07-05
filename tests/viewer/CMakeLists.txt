# Tests for viewer-side render math. rime_add_test (tests/CMakeLists.txt) builds the exe, links
# doctest, applies the build policy, and registers it with CTest. The orbit camera GRADUATED to
# engine/render at M5.5 (ADR-0016 rule 3), so its test now links rime::render; the STL loader is
# still viewer-owned, so that sample directory stays on the include path.
rime_add_test(rime_viewer_tests
    camera_test.cpp
    stl_test.cpp)

target_link_libraries(rime_viewer_tests
    PRIVATE rime::core rime::render)

target_include_directories(rime_viewer_tests
    PRIVATE ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer)
