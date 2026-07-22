# 11-lit-rooms — Milestone 10's "done when" (m10.8): the whole advanced-lighting stack on one scene
# (CSM + local spot shadows + clustered forward + SDF/DDGI global illumination + SSR), and opening a
# dividing wall visibly relights the dark room behind it. --headless self-checks the integrated beat
# off a Vulkan device (lavapipe on Linux CI); --serve streams it live over Track S0 for the dev-server
# → Mac path. Links rime::app (loop + render + ecs), rime::render (the lighting), rime::assets (the
# in-code box SDFs the probes trace), rime::stream + rime::platform (serve). No physics/destruction:
# the wall opens through the ADR-0032 C2 lighting-invalidation hooks a real destruction event drives
# (10-destructible-wall wires the full physics version), so this sample isolates the LIGHTING
# response.
add_executable(lit_rooms main.cpp)

target_link_libraries(lit_rooms
    PRIVATE rime::app rime::render rime::assets rime::stream rime::platform)

rime_apply_common_target_options(lit_rooms)

# M10's "done when," in CI: render the full stack, break the wall, and self-check that the dark room's
# floor brightens (a skip where no Vulkan device exists — Linux CI's lavapipe runs it for real). A
# generous DDGI convergence budget keeps the margin comfortable; the run is a few seconds on lavapipe.
add_test(NAME lit_rooms_headless COMMAND lit_rooms --headless --frames 16)
