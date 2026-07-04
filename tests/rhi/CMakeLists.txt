# Tests for rime::rhi. rime_add_test (tests/CMakeLists.txt) builds the exe, links doctest, applies
# the project build policy, and registers it with CTest; we add the module under test on top.
#
# Pixel-readback proofs share this exe: the offscreen-triangle test (M3.3 "first pixels"), the
# textured-quad test (M3.5, M3's headline "done when"), and the depth test (A1, depth attachment).
# Each needs (a) its shaders compiled to SPIR-V — the triangle/quad shaders are shared with the
# matching sample, so rime_add_shaders points at the sample's sources, while the depth proof owns its
# shaders here (it has no sample counterpart) — and (b) any shared render helper on the include path.
# The tests run on whatever GPU is present; set RIME_REQUIRE_VULKAN in the environment (CI does) to
# make a missing device a failure, not a skip.
rime_add_test(rime_rhi_tests
    device_test.cpp
    offscreen_triangle_test.cpp
    textured_quad_test.cpp
    depth_test.cpp
    pushconst_test.cpp
    mesh_offscreen_test.cpp
    volume_texture_test.cpp
    stencil_cap_test.cpp
    cap_offscreen_test.cpp
    warp_offscreen_test.cpp
    iso_offscreen_test.cpp
    streamlines_offscreen_test.cpp
    viscous_offscreen_test.cpp
    assembly_offscreen_test.cpp
    engine_offscreen_test.cpp
    ui_offscreen_test.cpp
    provenance_offscreen_test.cpp
    turntable_test.cpp
    chart_offscreen_test.cpp
    ubo_test.cpp)

target_link_libraries(rime_rhi_tests
    PRIVATE rime::rhi) # rime::core (the camera math the mesh proof uses) comes transitively via rhi

# Share each render helper + its shaders with the matching sample (one source of truth per proof).
target_include_directories(rime_rhi_tests
    PRIVATE ${CMAKE_SOURCE_DIR}/samples/01-hello-triangle
            ${CMAKE_SOURCE_DIR}/samples/02-textured-quad
            ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer)
rime_add_shaders(rime_rhi_tests
    ${CMAKE_SOURCE_DIR}/samples/01-hello-triangle/shaders/triangle.vert
    ${CMAKE_SOURCE_DIR}/samples/01-hello-triangle/shaders/triangle.frag
    ${CMAKE_SOURCE_DIR}/samples/02-textured-quad/shaders/quad.vert
    ${CMAKE_SOURCE_DIR}/samples/02-textured-quad/shaders/quad.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/depth.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/depth.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pushconst.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pushconst.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ubo.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ubo.frag
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/mesh.vert
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/mesh.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/volume.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/volume.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/stencil_mark.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/stencil_solid.frag
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/capmark.frag
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/cap.vert
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/cap.frag
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/warp.vert
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/warp.frag
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/iso.vert
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/iso.frag
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/streamline.vert
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/streamline.frag
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/ui.vert
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/ui.frag)
