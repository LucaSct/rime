# 03-icem-viewer — the ICEM viewer skeleton (brick B1): load a computed part (binary STL) and draw it
# depth-correct and lit, orbitable with the mouse. Windowed via a swapchain (default) or off-screen to
# a PPM (--offscreen / the no-display fallback). Links rime::rhi (agnostic — never sees Vulkan) and
# rime::platform (window + input + event loop), and reuses rime::core math via the headers. The viewer's
# header-only helpers (camera.hpp, stl.hpp, mesh_render.hpp) sit next to main.cpp; rime_add_shaders
# compiles shaders/mesh.{vert,frag} to SPIR-V at build time and embeds them as headers this target
# includes. These helpers + shaders are the single source of truth the render test also compiles.
add_executable(icem_viewer main.cpp)

target_link_libraries(icem_viewer
    PRIVATE rime::rhi rime::platform)

rime_add_shaders(icem_viewer
    shaders/mesh.vert
    shaders/mesh.frag
    shaders/legend.vert
    shaders/legend.frag
    shaders/capmark.frag
    shaders/cap.vert
    shaders/cap.frag)

rime_apply_common_target_options(icem_viewer)
