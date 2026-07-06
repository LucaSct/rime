# rime_app — the application framework & fixed-tick frame loop (M5.7, ADR-0023) — plus rime_hello,
# the Milestone-0 launcher it grew out of.
#
# rime_app ties the engine's modules into a runnable whole: it owns the JobSystem, the ECS World +
# sim Schedule, the FixedTimestep, and — optionally — an rhi::Device + render::RenderGraph, and runs
# the loop that steps the simulation at a FIXED tick decoupled from the render frame (the multiplayer
# seam, ADR-0023). It depends on module *interfaces* only, so removing a feature module still leaves
# a buildable app (guardrail #2).

add_library(rime_app STATIC
    src/application.cpp)

# Namespaced alias so dependents link rime::app (matching the other modules).
add_library(rime::app ALIAS rime_app)

target_include_directories(rime_app
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC because the app's public headers name these modules' types: World/Schedule (ecs), JobSystem
# (core), Event (platform), and RenderGraph*/Device* in FrameContext (render, which brings rhi + core
# transitively). rime::render is always a valid target — RIME_RHI_VULKAN only gates the backend
# *inside* rhi, so an app on a backend-less build links fine and simply gets a null device.
target_link_libraries(rime_app
    PUBLIC rime::core rime::ecs rime::platform rime::render)

rime_apply_common_target_options(rime_app)

# ── rime_hello — the minimal M0 launcher (kept) ──────────────────────────────────────────────────
# Proves the engine builds, links a module (rime::core), and runs — the seed rime_app grew from.
add_executable(rime_hello
    main.cpp)

target_link_libraries(rime_hello
    PRIVATE rime::core fmt::fmt)

rime_apply_common_target_options(rime_hello)
