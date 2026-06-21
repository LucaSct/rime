# Tests for rime::rhi. rime_add_test (tests/CMakeLists.txt) builds the exe, links doctest, applies
# the project build policy, and registers it with CTest; we add the module under test on top.
#
# Two pixel-readback proofs share this exe: the offscreen-triangle test (M3.3 "first pixels") and the
# textured-quad test (M3.5, and M3's headline "done when"). Each needs (a) its shaders compiled to
# SPIR-V — shared with the matching sample, so rime_add_shaders points at the sample's sources — and
# (b) that sample's render helper on the include path. The tests run on whatever GPU is present; set
# RIME_REQUIRE_VULKAN in the environment (CI does) to make a missing device a failure, not a skip.
rime_add_test(rime_rhi_tests
    device_test.cpp
    offscreen_triangle_test.cpp
    textured_quad_test.cpp)

target_link_libraries(rime_rhi_tests
    PRIVATE rime::rhi)

# Share each render helper + its shaders with the matching sample (one source of truth per proof).
target_include_directories(rime_rhi_tests
    PRIVATE ${CMAKE_SOURCE_DIR}/samples/01-hello-triangle
            ${CMAKE_SOURCE_DIR}/samples/02-textured-quad)
rime_add_shaders(rime_rhi_tests
    ${CMAKE_SOURCE_DIR}/samples/01-hello-triangle/shaders/triangle.vert
    ${CMAKE_SOURCE_DIR}/samples/01-hello-triangle/shaders/triangle.frag
    ${CMAKE_SOURCE_DIR}/samples/02-textured-quad/shaders/quad.vert
    ${CMAKE_SOURCE_DIR}/samples/02-textured-quad/shaders/quad.frag)
