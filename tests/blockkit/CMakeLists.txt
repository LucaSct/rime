# m13.2c — the block as content: assembly, determinism, the `.rscene` round trip, ADR-0035 §1's
# scale floors measured from the LOADED world, and the palette's coverage counter.
#
# GPU-free and asset-free by construction: assembly is pure data, the scene format carries placement
# and intent, and the only GPU-bound half of the palette (mesh upload) is a separate call. So this
# runs on every CI OS and under both sanitizers. Standing the block up in a real physics world needs
# cooked `.rdest` files and is the sibling proof, block_standup_test.
rime_add_test(blockkit_test blockkit_test.cpp)
target_link_libraries(blockkit_test
    PRIVATE rime::blockkit rime::scene rime::destruction rime::render rime::ecs rime::core)

# ── block_standup_test — the block standing in a real physics world (m13.2c) ─────────────────────
# Needs the nine cooked `.rdest` files, so it is driven by a cook fixture rather than a checked-in
# asset: `rime fracture` seeds them exactly as a user would, and the test re-reads each one to
# confirm the CMake cook list below still matches `blockkit::cook_specs()`. Still GPU-free — cooked
# geometry in, physics bodies out — so it runs on every CI OS and under both sanitizers.
rime_add_test(block_standup_test block_standup_test.cpp)
target_link_libraries(block_standup_test
    PRIVATE rime::blockkit rime::scene rime::destruction rime::physics rime::assets
            rime::platform rime::render rime::ecs rime::core)

# Its own cooked dir, per preset, so dev/asan/tsan never share cooked bytes and this proof can be
# run or debugged without another test's fixture in play.
set(BLOCK_COOKED_DIR "${CMAKE_CURRENT_BINARY_DIR}/cooked")
target_compile_definitions(block_standup_test PRIVATE RIME_BLOCK_COOKED_DIR="${BLOCK_COOKED_DIR}")

# The nine cooks. This list and `blockkit::cook_specs()` are two descriptions of the same thing;
# block_standup_test reads the produced files back and fails if they disagree, so a drift here is a
# red test rather than a quietly smaller block. Keep them in the same order for review.
#   name        size            parts  seed
#   wall_bg     8   3   0.3     12     1301
#   wall_hero   8   3   0.3     28     1310
#   side_bg     7.4 3   0.3     12     1303
#   side_hero   7.4 3   0.3     28     1304
#   floor_bg    7.4 0.3 7.4     12     1305
#   floor_hero  7.4 0.3 7.4     28     1306
#   half_bg     3   3   0.3      6     1307
#   half_hero   3   3   0.3     14     1308
#   crate       1   1   1        8     1309
set(BLOCK_COOKS
    "wall_bg|8|3|0.3|12|1301"
    "wall_hero|8|3|0.3|28|1310"
    "side_bg|7.4|3|0.3|12|1303"
    "side_hero|7.4|3|0.3|28|1304"
    "floor_bg|7.4|0.3|7.4|12|1305"
    "floor_hero|7.4|0.3|7.4|28|1306"
    "half_bg|3|3|0.3|6|1307"
    "half_hero|3|3|0.3|14|1308"
    "crate|1|1|1|8|1309")

set(BLOCK_COOK_FIXTURES "")
foreach(cook IN LISTS BLOCK_COOKS)
    # '|' rather than ';' as the field separator: a semicolon IS CMake's list separator, so a
    # nested ';'-joined record is flattened by foreach and every list(GET ...) below would index
    # into the wrong row.
    string(REPLACE "|" ";" cook "${cook}")
    list(GET cook 0 c_name)
    list(GET cook 1 c_x)
    list(GET cook 2 c_y)
    list(GET cook 3 c_z)
    list(GET cook 4 c_parts)
    list(GET cook 5 c_seed)
    add_test(NAME block_cook_${c_name}
        COMMAND cargo run --quiet --manifest-path tools/Cargo.toml --bin rime --
                fracture --size ${c_x} ${c_y} ${c_z} --parts ${c_parts} --seed ${c_seed}
                --out "${BLOCK_COOKED_DIR}" --name ${c_name}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(block_cook_${c_name} PROPERTIES FIXTURES_SETUP block_cook)
    list(APPEND BLOCK_COOK_FIXTURES block_cook_${c_name})
endforeach()

set_tests_properties(block_standup_test PROPERTIES FIXTURES_REQUIRED block_cook)

# ── block_render_test — the block DRAWN (m13.2d) ──────────────────────────────────────────────────
# The half of the render bridge that needs a device: shared part meshes (a claim about MeshIds,
# which only a real registry mints), the frustum cull doing non-trivial work in both directions at
# block scale, and the block's own pixels being brighter than the same camera with nothing to look
# at. Only built with the Vulkan backend, like tests/render; it reuses that suite's readback helper
# rather than copying it.
if(RIME_RHI_VULKAN)
    rime_add_test(block_render_test block_render_test.cpp)
    target_link_libraries(block_render_test
        PRIVATE rime::blockkit rime::destruction_render rime::scene rime::destruction
                rime::physics rime::assets rime::platform rime::render rime::rhi
                rime::ecs rime::core)
    target_include_directories(block_render_test PRIVATE ${CMAKE_SOURCE_DIR}/tests/render)
    target_compile_definitions(block_render_test PRIVATE RIME_BLOCK_COOKED_DIR="${BLOCK_COOKED_DIR}")
    set_tests_properties(block_render_test PROPERTIES FIXTURES_REQUIRED block_cook)
endif()
