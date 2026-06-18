# 01-hello-triangle — M3.3 proof: render a triangle off-screen through the RHI and preview it as
# ASCII. Links rime::rhi only (the agnostic interface — it never sees Vulkan). rime_add_shaders
# compiles shaders/triangle.{vert,frag} to SPIR-V at build time and embeds them as headers this
# target includes. (M3.4 upgrades this to present in a window via a swapchain.)
add_executable(hello_triangle main.cpp)

target_link_libraries(hello_triangle
    PRIVATE rime::rhi)

rime_add_shaders(hello_triangle
    shaders/triangle.vert
    shaders/triangle.frag)

rime_apply_common_target_options(hello_triangle)
