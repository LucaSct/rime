# 07-first-light — Milestone 5's "done when" (M5.8): an application-framework-driven lit PBR scene
# through the render graph. --headless self-checks off-screen (CI/lavapipe); --serve streams it live
# over Track S0 to the 04-remote-view client (the dev-server → Mac path); windowed is the ADR-0023 §4
# seam. Links rime::app (the fixed-tick loop + render + ecs, transitively) and rime::stream (the
# capture→encode→transport stack + sockets).
add_executable(first_light main.cpp)

target_link_libraries(first_light
    PRIVATE rime::app rime::stream)

rime_apply_common_target_options(first_light)

# M5's "done when," in CI: the headless self-check renders a few frames off-screen and asserts the
# scene is lit (green on lavapipe; a skip where no device exists unless RIME_REQUIRE_VULKAN is set).
# A small frame count keeps it quick under the sanitizers.
add_test(NAME first_light_headless COMMAND first_light --headless --frames 4)
