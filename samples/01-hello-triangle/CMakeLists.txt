# 01-hello-triangle — renders Rime's first triangle through the RHI. Two modes (see main.cpp):
# windowed presentation via a swapchain (M3.4, the default) and an off-screen ASCII preview (M3.3,
# --offscreen / the fallback when there's no display). Links rime::rhi (the agnostic interface — it
# never sees Vulkan) and rime::platform (for the window + event loop in windowed mode). rime_add_shaders
# compiles shaders/triangle.{vert,frag} to SPIR-V at build time and embeds them as headers we include.
add_executable(hello_triangle main.cpp)

target_link_libraries(hello_triangle
    PRIVATE rime::rhi rime::platform)

rime_add_shaders(hello_triangle
    shaders/triangle.vert
    shaders/triangle.frag)

rime_apply_common_target_options(hello_triangle)
