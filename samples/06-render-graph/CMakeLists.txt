# 06-render-graph — the render graph demonstrated (M5.8): a procedural PBR scene drawn through the
# M5.6 pass library (depth pre-pass → forward PBR → tonemap) on a render::RenderGraph, printing the
# compiled pass order + per-pass GPU time, and showing the milestone's "adding a pass is easy" claim
# via a runtime-toggled vignette post pass (--vignette). Headless/off-screen, so it runs on lavapipe
# and in CI. Links rime::render (which brings rhi + ecs + core transitively).
add_executable(render_graph_sample main.cpp)

# The demo post pass's shaders, compiled to SPIR-V and embedded (ADR-0008), same as every sample.
rime_add_shaders(render_graph_sample
    shaders/fullscreen.vert
    shaders/vignette.frag)

target_link_libraries(render_graph_sample
    PRIVATE rime::render)

rime_apply_common_target_options(render_graph_sample)

# The off-screen self-check doubles as a CI smoke test (green on lavapipe; a skip where no device
# exists unless RIME_REQUIRE_VULKAN is set). Run the vignette path so the extra pass is exercised too.
add_test(NAME render_graph_sample_smoke COMMAND render_graph_sample --headless --vignette)
