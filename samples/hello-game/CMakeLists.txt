# hello-game — the on-ramp sample ADR-0038 §m15.7 named and the M20 prerequisite ADR-0046 §1 made of
# it. The smallest complete game in the tree: walk, touch five trigger volumes, push a crate, win.
#
# Everything the game IS lives in game.hpp/.cpp as a `rime::app::Game`; main.cpp holds only this
# sample's self-checks. m20.1 cut the engine-owned `GameDefinition` along exactly that seam
# (ADR-0046 §1's "riskiest assumption", tested here on 500 lines instead of on the block's 3000).
#
# Links rime::app_game (the launch modes + the one loop they share; rime::app, render and ecs come
# transitively) and rime::physics (the kinematic character and the m15.6 sensor triggers the pickups
# actually use — the GAME's dependency, not the engine's).
add_executable(hello_game main.cpp game.cpp)

target_link_libraries(hello_game
    PRIVATE rime::app_game rime::app rime::physics rime::scene)

rime_apply_common_target_options(hello_game)

# m20.2 — the arena is CONTENT: `content/hello-game.rscene`, found at run time through the content
# root (`--content`, else `content/` beside the executable, else — for a binary still in its build
# directory only — this source directory). rime_game_content() hands the game those two build-tree
# paths and writes the export record `scripts/export-game.sh hello_game <out>` reads.
# This game's redistribution audit is deliberately target-scoped: other games can link codecs
# or ship third-party art. They must provide their own reviewed metadata before being packaged.
include(${CMAKE_CURRENT_SOURCE_DIR}/bundle-metadata.cmake)
rime_game_content(hello_game ${CMAKE_CURRENT_SOURCE_DIR}/content
    BUNDLE_METADATA "${HELLO_GAME_BUNDLE_METADATA}")

# The default run is the GPU-FREE self-check, so it gates on every CI OS and under both sanitizers —
# the rules, the character controller and the trigger volumes are all CPU work, and none of them
# needs a device to be proved. It asserts the scripted player wins AND that the identical scenario on
# a different worker-thread count reaches the identical result, marker by marker and tick by tick
# (ADR-0026 determinism; a scalar score would also match a run that collected them in another order).
add_test(NAME hello_game_selftest COMMAND hello_game --verbose)

# The rendered path, off-screen: green on lavapipe, an honest skip where no device exists unless
# RIME_REQUIRE_VULKAN says the machine should have one. Separate from the test above because this one
# proves a different thing — that the same game, unchanged, also draws.
add_test(NAME hello_game_headless COMMAND hello_game --headless --frames 8)

# m20.1 — the dedicated proof. The same game through the engine's `dedicated` mode and three other
# ways (the pre-port hand-rolled loop, `play` with a device factory that answers null, and — last,
# where a device exists — `play` with a real one) must reach the SAME state digest, mid-game at 120
# ticks and at the win. Dedicated must never enter the device factory (a counter, not a guess), and
# on Linux `/proc/self/maps` must show no libvulkan after every GPU-free leg.
add_test(NAME hello_game_dedicated_proof COMMAND hello_game --dedicated-proof --verbose)

# The shipped-game CLI end to end: a bounded dedicated run, and a stub mode that must refuse with
# the "not yet" exit code (3) rather than quietly running `play`.
add_test(NAME hello_game_cli_dedicated COMMAND hello_game dedicated --offline --ticks 120 --autopilot)
add_test(NAME hello_game_cli_browser_not_yet COMMAND hello_game browser --port 8443)
# PASS_REGULAR_EXPRESSION rather than WILL_FAIL: WILL_FAIL passes on ANY non-zero exit, a crash
# included. The exact exit code (3) is asserted by tests/app/launch_test.cpp.
set_tests_properties(hello_game_cli_browser_not_yet PROPERTIES
    PASS_REGULAR_EXPRESSION "'browser' mode is not implemented yet")

# m20.2 — a content root that is named and wrong is a refusal that says where it looked, never a run
# on an empty world. (The bundle-side half — a moved binary with its content deleted — is
# scripts/export-proof.sh, because it needs a binary outside the build tree.)
add_test(NAME hello_game_cli_content_missing
    COMMAND hello_game dedicated --offline --ticks 1 --content ${CMAKE_CURRENT_BINARY_DIR}/no-such-content)
set_tests_properties(hello_game_cli_content_missing PROPERTIES
    PASS_REGULAR_EXPRESSION "no content root found; tried, in order:.*no-such-content: missing")

# These process-level gates exercise the listener without a GPU or a network client. The Python
# helper only reads events and, for the conflict case, holds a local UDP port open.
add_test(NAME hello_game_listen_ready COMMAND python3 -c [=[
import json, subprocess, sys
p = subprocess.run([sys.argv[1], 'dedicated', '--listen', '127.0.0.1:0', '--ticks', '30'],
                   capture_output=True, text=True, timeout=10)
assert p.returncode == 0, (p.returncode, p.stdout, p.stderr)
lines = [json.loads(line) for line in p.stdout.splitlines()]
assert lines[0]['event'] == 'ready' and lines[0]['transport'] == 'udp'
assert lines[0]['auth'] == 'none' and int(lines[0]['bind'].rsplit(':', 1)[1]) > 0
assert lines[-1]['event'] == 'finished' and lines[-1]['ticks'] == 30
assert lines[-1]['synthesized'] == 30
]=] $<TARGET_FILE:hello_game>)
set_tests_properties(hello_game_listen_ready PROPERTIES TIMEOUT 15)

add_test(NAME hello_game_listen_bind_conflict COMMAND python3 -c [=[
import json, socket, subprocess, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', 0))
port = s.getsockname()[1]
p = subprocess.run([sys.argv[1], 'dedicated', '--listen', f'127.0.0.1:{port}', '--ticks', '1'],
                   capture_output=True, text=True, timeout=10)
assert p.returncode == 1, (p.returncode, p.stdout, p.stderr)
lines = [json.loads(line) for line in p.stdout.splitlines()]
assert len(lines) == 1 and lines[0]['event'] == 'bind_failed'
assert lines[0]['error'] == 'EADDRINUSE'
]=] $<TARGET_FILE:hello_game>)
set_tests_properties(hello_game_listen_bind_conflict PROPERTIES TIMEOUT 15)

# The process proof uses POSIX fork/exec and a pipe because engine/platform has no child-process
# helper. Compile the game rules into this test too so its replay uses the same implementation.
if(NOT WIN32)
    # The production executable on both sides, with a real loopback UDP port. A child-process
    # helper is not available on Windows yet, so this POSIX-only check uses Python subprocess.
    add_test(NAME hello_game_play_connect COMMAND python3
        ${CMAKE_CURRENT_SOURCE_DIR}/connect_prediction.py $<TARGET_FILE:hello_game>)
    set_tests_properties(hello_game_play_connect PROPERTIES TIMEOUT 120 LABELS "hello_game")
    # m20.5c2: the Throw's two-process proof -- faults, response, and the sensitivity controls
    # that must fail. 13 sequential legs; TIMEOUT is the measured runtime times three.
    add_test(NAME hello_game_play_throw COMMAND python3
        ${CMAKE_CURRENT_SOURCE_DIR}/throw_prediction.py $<TARGET_FILE:hello_game>)
    # RUN_SERIAL: two real-time processes whose press must land inside the server's one-command input
    # buffer; a parallel suite starving them is what the proof's rerun exists for, so do not invite it.
    set_tests_properties(hello_game_play_throw PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE LABELS "hello_game")
    # m20.6c: the interaction group's two-process proof -- scripted membership, a server-only kick,
    # the undisturbed residual, response, and three controls that must fail. 13 sequential legs,
    # 122 s measured; TIMEOUT is the measured runtime times three. RUN_SERIAL for the same reason as
    # the throw proof: a parallel suite starving a leg only costs a rerun, so do not invite it.
    add_test(NAME hello_game_play_interaction COMMAND python3
        ${CMAKE_CURRENT_SOURCE_DIR}/interaction_prediction.py $<TARGET_FILE:hello_game>)
    set_tests_properties(hello_game_play_interaction PROPERTIES TIMEOUT 400 RUN_SERIAL TRUE LABELS "hello_game")
    add_executable(hello_game_net_proof net_proof.cpp game.cpp)
    target_link_libraries(hello_game_net_proof
        PRIVATE rime::app_game rime::app rime::physics rime::scene rime::net rime::replication)
    target_compile_definitions(hello_game_net_proof PRIVATE
        RIME_GAME_CONTENT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/content"
        RIME_GAME_BINARY_DIR="${CMAKE_BINARY_DIR}/bin")
    rime_apply_common_target_options(hello_game_net_proof)
    add_dependencies(hello_game_net_proof hello_game)
    add_test(NAME hello_game_net_proof COMMAND hello_game_net_proof)
    add_test(NAME hello_game_net_proof_negative COMMAND hello_game_net_proof --negative)
    set_tests_properties(hello_game_net_proof PROPERTIES TIMEOUT 25 LABELS "hello_game")
    set_tests_properties(hello_game_net_proof_negative PROPERTIES TIMEOUT 75 LABELS "hello_game")
else()
    message(STATUS "hello_game_net_proof, hello_game_play_connect, hello_game_play_throw and hello_game_play_interaction excluded on Windows: no child-process helper")
endif()
