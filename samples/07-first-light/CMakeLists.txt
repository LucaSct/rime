# 07-first-light — Milestone 5's "done when" (M5.8): an application-framework-driven lit PBR scene
# through the render graph. --headless self-checks off-screen (CI/lavapipe); --serve streams it live
# over Track S0 to the 04-remote-view client (the dev-server → Mac path); windowed is the ADR-0023 §4
# seam. Links rime::app (the fixed-tick loop + render + ecs, transitively) and rime::stream (the
# capture→encode→transport stack + sockets).
add_executable(first_light main.cpp)

# rime::gameplay arrives with m13.3c: the windowed run drives a gameplay::FlyCamera from the frame's
# input snapshot, which is what turned the window from a fixed shot into something you can walk in.
target_link_libraries(first_light
    PRIVATE rime::app rime::stream rime::gameplay)

rime_apply_common_target_options(first_light)

# M5's "done when," in CI: the headless self-check renders a few frames off-screen and asserts the
# scene is lit (green on lavapipe; a skip where no device exists unless RIME_REQUIRE_VULKAN is set).
# A small frame count keeps it quick under the sanitizers.
add_test(NAME first_light_headless COMMAND first_light --headless --frames 4)

# m13.3c, and the reason it is a CTest on the BINARY rather than a unit test: the engine join is
# proved in tests/app/windowed_input_test.cpp, but only running this executable can show that
# run_windowed() actually builds a FlyCamera and feeds it the frame's input. m13.3a shipped with
# every part of that chain built, all of it green, and the last link missing — so the last link is
# what this checks. Degrades honestly with no device, like the case above.
add_test(NAME first_light_windowed_input COMMAND first_light --input-selftest)
