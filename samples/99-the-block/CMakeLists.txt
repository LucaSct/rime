# 99-the-block — Milestone 13's "done when" and the vision demo: a destructible urban block with the
# full lighting stack, replicated, with a predicted first-person player in it. See main.cpp's header
# for what it asserts and what it deliberately leaves to the samples that already prove it.
#
# It links nearly the whole engine, which is the point — this is the first binary that holds M8, M10,
# M11, M12 and M13 at once.
add_executable(the_block main.cpp)

target_link_libraries(the_block
    PRIVATE rime::app
            rime::worldkit
            rime::blockkit
            rime::scene
            rime::destruction
            rime::destruction_net
            rime::destruction_render
            rime::gameplay
            rime::gameplay_net
            rime::replication
            rime::net
            rime::physics
            rime::assets
            rime::audio
            rime::render
            rime::platform
            rime::ecs
            rime::core)

rime_apply_common_target_options(the_block)

# ── The demo's own editor host (m15.2) ──────────────────────────────────────────────────────────
# The whole point of ADR-0038's §2: a game that wants its components in the editor's inspector
# builds its own host from the engine's library and adds its module on top. Nine lines of C++, one
# CMake target, and nothing in engine/ knows this game exists. Point the editor at it with
# `editor --engine build/dev/bin/the-block-host`.
add_executable(the_block_host block_host_main.cpp)
set_target_properties(the_block_host PROPERTIES OUTPUT_NAME the-block-host)
target_link_libraries(the_block_host
    PRIVATE rime::editorhost_app rime::worldkit rime::blockkit rime::ecs rime::render
            rime::destruction)
rime_apply_common_target_options(the_block_host)

# ── The cooked block ────────────────────────────────────────────────────────────────────────────
# The nine `.rdest` files this consumes are produced by `rime fracture`, exactly as a user would.
# Its OWN cooked dir, per preset, so dev/asan/tsan never share cooked bytes and this demo can be run
# or debugged without a test fixture in play. `--cooked <dir>` overrides it for a hand-run.
set(BLOCK_DEMO_COOKED "${CMAKE_CURRENT_BINARY_DIR}/cooked")
target_compile_definitions(the_block PRIVATE RIME_BLOCK_COOKED_DIR="${BLOCK_DEMO_COOKED}")

# This list and `blockkit::cook_specs()` are two descriptions of one thing; the demo reads the
# produced files back and refuses to start if they disagree, so a drift here is a loud failure
# rather than a quietly smaller block. Same nine rows as tests/blockkit, in the same order.
# DAMAGE TUNING IS PER COOK, and m13.5 is why. `damage_threshold` is the contact impulse a part
# absorbs before it takes damage, and it is a property of the part's MASS — the fracturer's 5.0
# default was tuned for M8's small test wall. A building slab part is two orders of magnitude
# heavier: measured on the block, contact impulses reach 669 kg·m/s, and at threshold 5 with parts
# standing at 1.0 health, 729 of 799 contact ops were INSTANT KILLS. One demolition charge then
# flattened the whole block, across a 12 m gap and then the street. Explicit apply_damage (a weapon,
# a charge) does not go through this path, so these numbers do not make a wall bulletproof.
#   name        size            parts  seed  dmg-threshold  dmg-scale
set(BLOCK_DEMO_COOKS
    "wall_bg|8|3|0.3|12|1301|20|0.0015"
    "wall_hero|8|3|0.3|28|1302|20|0.0015"
    "side_bg|7.4|3|0.3|12|1303|20|0.0015"
    "side_hero|7.4|3|0.3|28|1304|20|0.0015"
    "floor_bg|7.4|0.3|7.4|12|1305|20|0.0015"
    "floor_hero|7.4|0.3|7.4|28|1306|20|0.0015"
    "half_bg|3|3|0.3|6|1307|12|0.0025"
    "half_hero|3|3|0.3|14|1308|12|0.0025"
    "crate|1|1|1|8|1309|4|0.02")

foreach(cook IN LISTS BLOCK_DEMO_COOKS)
    # '|' and not ';': a semicolon IS CMake's list separator, so a ';'-joined record is flattened by
    # foreach and every list(GET) below would index into the wrong row.
    string(REPLACE "|" ";" cook "${cook}")
    list(GET cook 0 c_name)
    list(GET cook 1 c_x)
    list(GET cook 2 c_y)
    list(GET cook 3 c_z)
    list(GET cook 4 c_parts)
    list(GET cook 5 c_seed)
    list(GET cook 6 c_dthr)
    list(GET cook 7 c_dscale)
    add_test(NAME block_demo_cook_${c_name}
        COMMAND cargo run --quiet --manifest-path tools/Cargo.toml --bin rime --
                fracture --size ${c_x} ${c_y} ${c_z} --parts ${c_parts} --seed ${c_seed}
                --damage-threshold ${c_dthr} --damage-scale ${c_dscale}
                --out "${BLOCK_DEMO_COOKED}" --name ${c_name}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(block_demo_cook_${c_name} PROPERTIES FIXTURES_SETUP block_demo_cook)
endforeach()

# M13's "done when", in CI. The sim half runs anywhere (it is GPU-free); the render assertions are
# self-guarded on a device, and a host without one is an honest skip rather than a red build —
# unless RIME_REQUIRE_VULKAN says otherwise.
#
# Deliberately NOT a timing gate, and there is no --perf mode yet: the clock belongs on hardware
# whose fingerprint goes into a report, never on a CI rasteriser, and its scope should come from
# this demo rather than from a guess about it. That is m13.p.
#
# One claim is REPORTED AND NOT ASSERTED — the collapse does not stay local — so this test passes
# while M13 stays open. See main.cpp's KNOWN DEFECT block; it becomes claim #21 when it is fixed.
add_test(NAME the_block_headless COMMAND the_block --headless)
set_tests_properties(the_block_headless PROPERTIES
    FIXTURES_REQUIRED block_demo_cook
    TIMEOUT 600)
