# 14-virtual-geometry — the complexity sweep ADR-0043 gate 4 asks for: candidates, selected
# triangles, CPU submission and GPU time at a fixed projected size, over a generated quadtree whose
# depth is the only variable. Needs the Vulkan backend (it measures the GPU-built draw path) and
# rime::render for the selection/visibility passes; rime::core carries the perf report and ledger.
add_executable(virtual_geometry main.cpp)

target_link_libraries(virtual_geometry
    PRIVATE rime::render rime::assets rime::rhi rime::core)

rime_apply_common_target_options(virtual_geometry)
