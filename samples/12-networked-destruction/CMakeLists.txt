# 12-networked-destruction — Milestone 11's "done when" (M11.7): a dedicated headless server runs the
# canonical destruction sim while two clients at different viewpoints observe, and all three must
# agree on `destruction_net::shared_state_hash` despite provably receiving different bytes. GPU-free
# — cooked geometry in, physics bodies and packets out — so it runs on every CI OS plus the
# sanitizers. Links rime::destruction_net (the module under proof), rime::replication + rime::net
# (the transport it rides), rime::destruction + rime::physics (the sim), and rime::assets +
# rime::platform (read cooked RMA1).
add_executable(networked_destruction main.cpp)

target_link_libraries(networked_destruction
    PRIVATE rime::destruction_net rime::replication rime::net
            rime::destruction rime::physics rime::ecs
            rime::assets rime::platform rime::core)

rime_apply_common_target_options(networked_destruction)

# The cooked wall, one per preset so dev/asan/tsan never share cooked bytes.
set(NETWALL_COOKED_DIR "${CMAKE_CURRENT_BINARY_DIR}/cooked")
target_compile_definitions(networked_destruction PRIVATE RIME_WALL_COOKED_DIR="${NETWALL_COOKED_DIR}")

# The cook fixture: `rime fracture` seeds the Voronoi wall before the proof, exactly as a user would.
# Deliberately its own cooked dir rather than sharing 10-destructible-wall's: two tests that consume
# one fixture cannot be run or debugged independently, and a change to the wall's part count would
# silently retune this proof's byte budget.
add_test(NAME networked_destruction_cook
    COMMAND cargo run --quiet --manifest-path tools/Cargo.toml --bin rime --
            fracture --size 4 3 0.3 --parts 60 --seed 11 --out "${NETWALL_COOKED_DIR}" --name wall
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(networked_destruction_cook PROPERTIES FIXTURES_SETUP netwall_cook)

# M11's "done when", in CI: the deterministic scripted-network run. Exact counters, exact packet
# economy, and a negative control that must MISMATCH — see README.md for what this can and cannot
# claim versus the UDP variant.
add_test(NAME networked_destruction_headless
    COMMAND networked_destruction --headless --cooked "${NETWALL_COOKED_DIR}")
set_tests_properties(networked_destruction_headless PROPERTIES FIXTURES_REQUIRED netwall_cook)

# The same match over real loopback UDP sockets. A convergence smoke: it asserts agreement and
# nothing about counts, because the OS owns the clock here. It exists because the scripted run
# cannot see a socket bug — that is the one blind spot a virtual network has by construction.
add_test(NAME networked_destruction_udp
    COMMAND networked_destruction --headless --transport=udp --cooked "${NETWALL_COOKED_DIR}")
set_tests_properties(networked_destruction_udp PROPERTIES FIXTURES_REQUIRED netwall_cook)
