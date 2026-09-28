# target-range — Milestone 15's "done when" (m15.8, ADR-0038): the PLATFORM PROOF.
#
#   a small game that is NOT the block is authored through the editor and runs on the engine, with no
#   engine or editor source changed to support it
#
# The proof is partly what this directory does not contain and partly what the milestone's diff does
# not touch. The game defines its own reflected component (`targetrange::Target`), writes it into a
# `.rscene`, and the engine's serializer round-trips it without knowing what a target is — because
# `worldkit::register_engine_components` names the engine's components and stops there.
#
# Links rime::app (loop + render + ecs), rime::physics (the raycast a shot really is), rime::scene
# (the .rscene reader) and rime::worldkit (the engine's component profile).
add_executable(target_range main.cpp)

target_link_libraries(target_range
    PRIVATE rime::app rime::physics rime::scene rime::worldkit)

rime_apply_common_target_options(target_range)

# The game's OWN editor host (the m15.2 seam). Without it the editor cannot save this game's scene at
# all: `rime-engine` has never heard of `targetrange::Target` and correctly REFUSES to save a world
# whose load skipped component types, because saving would delete the authored data. Point the editor
# at this binary instead — `editor --engine build/dev/bin/target-range-host` — and it understands the
# range. Three lines of registrar plus a viewport preparer, and no engine change.
add_executable(target_range_host host_main.cpp)
set_target_properties(target_range_host PROPERTIES OUTPUT_NAME target-range-host)
target_link_libraries(target_range_host
    PRIVATE rime::editorhost_app rime::worldkit rime::ecs rime::render rime::physics)
rime_apply_common_target_options(target_range_host)

set(target_range_scene "${CMAKE_CURRENT_SOURCE_DIR}/target_range.rscene")

# M15's "done when", in CI, and GPU-free so it gates on all three OSes and under both sanitizers. It
# asserts, in order: the shipped scene loads STRICTLY (zero skipped components — the engine understood
# the game's own data), the scene declared targets at all (so winning is not vacuous), every target
# was knocked down by real raycasts, and the whole run is identical on a different worker-thread count
# including the placement digest (ADR-0026).
add_test(NAME target_range_selftest
         COMMAND target_range --scene "${target_range_scene}" --verbose)

# The same authored scene, rendered off-screen. Green on lavapipe, an honest skip where no device
# exists unless RIME_REQUIRE_VULKAN says the machine should have one.
add_test(NAME target_range_headless
         COMMAND target_range --scene "${target_range_scene}" --headless --frames 8)

# THE AUTHORING LOOP, on a game that is not the block (M14 proved it for the block only).
#
# `editor --smoke --scene <file>` opens the scene, edits it and saves it through the engine's own host;
# the game then runs the SAVED file and must still win, with a DIFFERENT placement digest. The digest
# clause is the load-bearing one: "the game ran the file the editor saved" is equally true of a game
# that regenerated its own level and ignored the file, and only two differing digests prove otherwise.
#
# Guarded on the editor binary existing, because the Rust half is a separate toolchain and this
# sample's C++ test must not fail on a --cpp-only build. CI's "Editor smoke" job has both.
add_test(NAME target_range_authoring_round_trip
         COMMAND "${CMAKE_COMMAND}"
                 -DTARGET_RANGE=$<TARGET_FILE:target_range>
                 -DSCENE=${target_range_scene}
                 -DEDITOR=${CMAKE_SOURCE_DIR}/tools/target/debug/editor
                 -DENGINE=$<TARGET_FILE:target_range_host>
                 -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/round_trip.cmake")
