# Tests for rime::render — the render graph (M5.4+). Pixel-verified through the Vulkan backend on
# lavapipe (GPU-free), like tests/rhi; only built when the backend is (the graph interface itself
# is API-agnostic, but a proof needs a device). The pass bodies reuse the rhi test shaders — the
# fullscreen triangle, the push-constant fill, the texel-perfect sampler copy, and the compute
# pattern — one source of truth per shader, same as the sample/test sharing everywhere else.
#
# viewer_frame_graph_test (M5.9) is the dogfood-acceptance proof: it re-expresses the ICEM viewer's
# cross-section frame as render-graph passes, so it shares the viewer's own headers + shaders the
# way tests/rhi does (samples/03-icem-viewer on the include path; its mesh/cap/ui shaders compiled
# here). One source of truth per shader — never a copy.
rime_add_test(rime_render_tests
    render_graph_test.cpp
    scene_layer_test.cpp
    pbr_pipeline_test.cpp
    pick_test.cpp
    viewer_frame_graph_test.cpp)

target_link_libraries(rime_render_tests
    PRIVATE rime::render)

target_include_directories(rime_render_tests
    PRIVATE ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer)

# The GPU asset-bridge proof loads the committed cooked checker.rtex through the AssetServer + bridge;
# hand it the same fixtures dir the engine/assets tests use (CMake emits forward slashes on every OS).
target_compile_definitions(rime_render_tests
    PRIVATE RIME_ASSETS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/assets/fixtures")

rime_add_shaders(rime_render_tests
    ${CMAKE_SOURCE_DIR}/tests/rhi/shaders/pushconst.vert
    ${CMAKE_SOURCE_DIR}/tests/rhi/shaders/pushconst.frag
    ${CMAKE_SOURCE_DIR}/tests/rhi/shaders/sample_storage.frag
    ${CMAKE_SOURCE_DIR}/tests/rhi/shaders/pattern.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh_flat.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh_flat.frag
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/mesh.vert
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/mesh.frag
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/capmark.frag
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/cap.vert
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/cap.frag
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/ui.vert
    ${CMAKE_SOURCE_DIR}/samples/03-icem-viewer/shaders/ui.frag)
