# 15-terrain — m19.8e's travel budget (ADR-0073): a scripted fly-through at walk, vehicle and
# aircraft speeds over a cooked km-scale world (make_world.sh), reporting terrain GPU ms, CPU ms
# for selection and residency, upload bytes, resident bytes, fallback draws and upload-cap waits,
# and the fastest speed whose fallback stays under 1%. Needs the Vulkan backend.
add_executable(terrain_flythrough main.cpp)

target_link_libraries(terrain_flythrough
    PRIVATE rime::render rime::assets rime::rhi rime::core rime::platform)

# The two terrain layers and the material the perf world is painted with are the committed
# splat-world fixture's palette (make_world.sh cooks the world against it).
target_compile_definitions(terrain_flythrough
    PRIVATE RIME_SPLAT_PALETTE_DIR="${CMAKE_SOURCE_DIR}/tests/assets/fixtures/splat_world/palette")

rime_apply_common_target_options(terrain_flythrough)
