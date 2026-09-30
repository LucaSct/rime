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
    PRIVATE rime::app_game rime::app rime::physics)

rime_apply_common_target_options(hello_game)

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
add_test(NAME hello_game_cli_dedicated COMMAND hello_game dedicated --ticks 120 --autopilot)
add_test(NAME hello_game_cli_browser_not_yet COMMAND hello_game browser --port 8443)
# PASS_REGULAR_EXPRESSION rather than WILL_FAIL: WILL_FAIL passes on ANY non-zero exit, a crash
# included. The exact exit code (3) is asserted by tests/app/launch_test.cpp.
set_tests_properties(hello_game_cli_browser_not_yet PROPERTIES
    PASS_REGULAR_EXPRESSION "'browser' mode is not implemented yet")
