# m13.2d — per-part render leaves. The leaf LIFE CYCLE (created once per part, following the sim
# through standing → detached → frozen, despawned on m13.2b's visual eviction) is bookkeeping over
# transforms, so it needs no device and runs on every CI OS plus both sanitizers. Only "the pixels
# are there" needs a GPU, and that half lives in tests/render.
rime_add_test(rime_destruction_render_tests part_leaves_test.cpp)

target_link_libraries(rime_destruction_render_tests
    PRIVATE rime::destruction_render rime::destruction rime::physics rime::assets
            rime::render rime::ecs rime::core)

# The committed m8.1 cooked wall, shared with the destruction and assets suites.
target_compile_definitions(rime_destruction_render_tests
    PRIVATE RIME_DESTRUCTION_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/assets/fixtures")
