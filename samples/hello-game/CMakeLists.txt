# hello-game — the on-ramp sample ADR-0038 §m15.7 named and the M20 prerequisite ADR-0046 §1 made of
# it. The smallest complete game in the tree: walk, touch five trigger volumes, push a crate, win.
#
# Everything the game IS lives in one `HelloGame` class with three methods; everything about RUNNING
# it lives in the host code beside it. That seam is where M20's engine-owned `GameDefinition` will be
# cut, and ADR-0046's "riskiest assumption" is that the cut is possible — which this sample exists to
# make testable on 500 lines instead of on the block's 3000.
#
# Links rime::app (the fixed-tick loop + render + ecs transitively) and rime::physics (the kinematic
# character and the m15.6 sensor triggers the pickups actually use).
add_executable(hello_game main.cpp)

target_link_libraries(hello_game
    PRIVATE rime::app rime::physics)

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
