# Tests for rime::rhi. rime_add_test (tests/CMakeLists.txt) builds the exe, links doctest, applies
# the project build policy, and registers it with CTest; we add the module under test on top.
#
# The offscreen-triangle test is the M3.3 "first pixels" proof. It needs (a) the triangle shaders
# compiled to SPIR-V — shared with the 01-hello-triangle sample, so we point rime_add_shaders at the
# sample's shader sources — and (b) the sample's render helper on the include path. The tests run on
# whatever GPU is present; set RIME_REQUIRE_VULKAN in the environment (CI does) to make a missing
# device a failure instead of a skip.
rime_add_test(rime_rhi_tests
    device_test.cpp
    offscreen_triangle_test.cpp)

target_link_libraries(rime_rhi_tests
    PRIVATE rime::rhi)

# Share the render helper + triangle shaders with samples/01-hello-triangle (one source of truth).
target_include_directories(rime_rhi_tests
    PRIVATE ${CMAKE_SOURCE_DIR}/samples/01-hello-triangle)
rime_add_shaders(rime_rhi_tests
    ${CMAKE_SOURCE_DIR}/samples/01-hello-triangle/shaders/triangle.vert
    ${CMAKE_SOURCE_DIR}/samples/01-hello-triangle/shaders/triangle.frag)
