# m19.3 — the terrain heightfield DRAWS, and it draws the surface physics collides with
# (ADR-0062-m19.3-terrain-render).
#
# The only test target in the tree that needs `rime::render` AND `rime::physics` AND a Vulkan
# device at the same time, which is why it is its own directory rather than a file in tests/render:
# the claim under test straddles the two modules, and nothing in the engine joins them (physics
# does not depend on engine/assets and never will — ADR-0060 §2). The glue — one
# `std::vector<std::uint16_t>` handed to a `HeightfieldAsset` and to a `HeightfieldDesc` — lives in
# the test because that is exactly the claim: ONE set of integers, TWO consumers.
#
# Rendered on lavapipe, GPU-free like every proof since M3.3, and structural: heights compared
# against a physics raycast within a derived margin, radiance against the analytic Lambert value.
# No golden images.
rime_add_test(rime_terrain_render_tests terrain_height_test.cpp)

target_link_libraries(rime_terrain_render_tests
    PRIVATE rime::render rime::physics rime::assets rime::core)

# The ENGINE's own terrain vertex stage, compiled into this target — one source of truth per
# shader, the same sharing tests/render does for the viewer's and the RHI's shaders. The proof
# pairs it with a test-owned fragment stage that reports the reconstructed height losslessly
# (floatBitsToUint into an R32_UINT target); pairing it with a COPY of the vertex shader would
# prove something about the copy.
rime_add_shaders(rime_terrain_render_tests
    ${CMAKE_SOURCE_DIR}/engine/render/shaders/terrain.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/terrain_height_probe.frag)
