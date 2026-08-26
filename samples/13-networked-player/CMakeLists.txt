# 13-networked-player — Milestone 12's "done when" (M12 "The Player", ADR-0036).
#
# A headless server and two clients on a scripted lossy link. The two clients drive the same tape
# and differ in exactly one thing: one predicts, one does not — so the prediction-off control that
# ADR-0035 §1 demands is a PEER IN THE SAME MATCH rather than a second run whose numbers were never
# comparable.
#
# GPU-free by construction: commands in, authoritative motion and packets out. It runs on every CI
# OS plus the sanitizers, and it never opens a window — the playable client is M13's (m13.3).
add_executable(networked_player main.cpp)

target_link_libraries(networked_player
    PRIVATE rime::gameplay_net rime::gameplay rime::replication rime::net
            rime::destruction rime::physics rime::ecs
            rime::assets rime::platform rime::core)

rime_apply_common_target_options(networked_player)

# The cooked wall, one per preset so dev/asan/tsan never share cooked bytes.
set(PLAYER_COOKED_DIR "${CMAKE_CURRENT_BINARY_DIR}/cooked")
target_compile_definitions(networked_player PRIVATE RIME_WALL_COOKED_DIR="${PLAYER_COOKED_DIR}")

# The cook fixture: `rime fracture` seeds the wall before the proof, exactly as a user would. Its
# own cooked dir rather than sharing another sample's — two tests that consume one fixture cannot be
# run or debugged independently, and a change to the wall's part count would silently retune this
# proof's "the wall lost parts" assertion.
add_test(NAME networked_player_cook
    COMMAND cargo run --quiet --manifest-path tools/Cargo.toml --bin rime --
            fracture --size 3 3 0.3 --parts 40 --seed 12 --out "${PLAYER_COOKED_DIR}" --name wall
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(networked_player_cook PROPERTIES FIXTURES_SETUP player_cook)

# M12's "done when", in CI. Self-gating: the binary returns non-zero if any clause fails, so the
# clauses live in the program rather than in a CMake assertion nobody reads.
add_test(NAME networked_player_headless
    COMMAND networked_player --headless --cooked "${PLAYER_COOKED_DIR}")
set_tests_properties(networked_player_headless PROPERTIES FIXTURES_REQUIRED player_cook)
