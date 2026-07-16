# 10-destructible-wall — Milestone 8's "done when" (M8.6): a cooked destructible wall stands, takes a
# hit, sheds tumbling debris, and fans ONE destruction event stream out to a VFX dust puff, the null
# audio backend, and gameplay. --headless self-checks the destruction sim off-screen (a GPU-free
# core) plus a pixel proof where a device exists; --serve streams it live over Track S0. Links
# rime::app (loop + render + ecs), rime::physics + rime::destruction (the sim), rime::vfx + rime::audio
# (the fan-out consumers), rime::stream (serve), and rime::assets + rime::platform (read cooked RMA1).
add_executable(destructible_wall main.cpp)

target_link_libraries(destructible_wall
    PRIVATE rime::app rime::physics rime::destruction rime::vfx rime::audio
            rime::stream rime::assets rime::platform)

rime_apply_common_target_options(destructible_wall)

# The cooked wall the CTest fixture fractures and the sample defaults to (overridable with --cooked).
# In the build tree, one per preset, so dev/asan/tsan never share cooked bytes.
set(WALL_COOKED_DIR "${CMAKE_CURRENT_BINARY_DIR}/cooked")
target_compile_definitions(destructible_wall PRIVATE RIME_WALL_COOKED_DIR="${WALL_COOKED_DIR}")

# The cook fixture: `rime fracture` seeds the Voronoi wall before the self-check, exactly as a user
# would (the recommended cook-at-test-fixture path — configure stays fast, CI exercises the real CLI).
# It needs the rime CLI, which cargo builds first (build.sh / the CI workflow build the Rust before
# ctest runs); `cargo run` is a no-op rebuild when it is already built.
add_test(NAME destructible_wall_cook
    COMMAND cargo run --quiet --manifest-path tools/Cargo.toml --bin rime --
            fracture --size 4 3 0.3 --parts 60 --seed 11 --out "${WALL_COOKED_DIR}" --name wall
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(destructible_wall_cook PROPERTIES FIXTURES_SETUP wall_cook)

# M8's "done when," in CI: hit the wall and self-check that parts die, an island detaches, the debris
# settles, all three fan-out consumers fire, and the whole run is bit-reproducible across physics
# worker counts — plus a pixel proof where a device exists (a skip otherwise, unless
# RIME_REQUIRE_VULKAN is set). FIXTURES_REQUIRED makes CTest run (and gate on) the cook first.
add_test(NAME destructible_wall_headless
    COMMAND destructible_wall --headless --cooked "${WALL_COOKED_DIR}")
set_tests_properties(destructible_wall_headless PROPERTIES FIXTURES_REQUIRED wall_cook)
