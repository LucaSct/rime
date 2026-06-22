# Tests for viewer-side render math. rime_add_test (tests/CMakeLists.txt) builds the exe, links
# doctest, applies the build policy, and registers it with CTest. The orbit camera is pure rime::core
# math that lives with the ICEM viewer sample, so we put that directory on the include path (the same
# "a test compiles a header the sample owns" pattern the rhi tests use) and link rime::core. As more
# viewer math lands (colormaps, streamline integration), its tests join this target.
rime_add_test(rime_render_tests
    camera_test.cpp)

target_link_libraries(rime_render_tests
    PRIVATE rime::core)

target_include_directories(rime_render_tests
    PRIVATE ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer)
