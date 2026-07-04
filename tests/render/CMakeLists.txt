# Tests for rime::render — the render graph (M5.4+). Pixel-verified through the Vulkan backend on
# lavapipe (GPU-free), like tests/rhi; only built when the backend is (the graph interface itself
# is API-agnostic, but a proof needs a device). The pass bodies reuse the rhi test shaders — the
# fullscreen triangle, the push-constant fill, the texel-perfect sampler copy, and the compute
# pattern — one source of truth per shader, same as the sample/test sharing everywhere else.
rime_add_test(rime_render_tests
    render_graph_test.cpp
    scene_layer_test.cpp)

target_link_libraries(rime_render_tests
    PRIVATE rime::render)

rime_add_shaders(rime_render_tests
    ${CMAKE_SOURCE_DIR}/tests/rhi/shaders/pushconst.vert
    ${CMAKE_SOURCE_DIR}/tests/rhi/shaders/pushconst.frag
    ${CMAKE_SOURCE_DIR}/tests/rhi/shaders/sample_storage.frag
    ${CMAKE_SOURCE_DIR}/tests/rhi/shaders/pattern.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh_flat.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh_flat.frag)
