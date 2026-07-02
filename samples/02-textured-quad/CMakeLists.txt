# 02-textured-quad — M3.5 proof (M3's "done when"): an indexed quad sampling a 2x2 texture, rendered
# through the RHI. Windowed via a swapchain (default) or off-screen with a colored-letter preview
# (--offscreen / the fallback where there's no display). Links rime::rhi (agnostic — never sees
# Vulkan) and rime::platform (window + event loop). rime_add_shaders compiles shaders/quad.{vert,frag}
# to SPIR-V at build time and embeds them as headers this target includes.
add_executable(textured_quad main.cpp)

target_link_libraries(textured_quad
    PRIVATE rime::rhi rime::platform)

rime_add_shaders(textured_quad
    shaders/quad.vert
    shaders/quad.frag)

rime_apply_common_target_options(textured_quad)
